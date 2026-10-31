//! Speed Demon 2026, Track B (Index), in Rust with no dependencies.
//!
//! 1. The queries are read first, and only their terms are looked for.
//! 2. Each thread samples the start of its share of docs.txt, counting the
//!    documents that hold each term: a BOOL term in over 1/32 of them is kept
//!    as a bitmap, and each phrase's rarest term becomes its anchor.
//! 3. The full scan. Every phrase is checked wherever its anchor occurs, while
//!    the text is in cache. A BOOL term in a document sets its bitmap bit or
//!    files the pair under the term's bucket; the buckets are then
//!    counting-sorted into per-thread posting lists of 16-bit doc offsets.
//! 4. BOOL queries run left to right on sorted lists, or on a bitmap once a
//!    bitmap term starts the query or is OR-ed in.
use std::arch::x86_64::*;
use std::fmt::Write;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering::Relaxed};
use std::sync::Mutex;

extern "C" {
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    fn mallopt(param: i32, value: i32) -> i32;
}

const THREADS: usize = 12;
const NONE: u32 = u32::MAX;
const BUCKET: usize = 11; // terms per bucket of scan output: 2^11
const GAP: u32 = 31; // a unit's 5-bit doc gap; GAP means the whole gap follows in the next unit
const DOC_BITS: u32 = 15; // a thread's share holds under 2^15 documents

/// The file, mapped over a spare zero page so loads of 32 bytes past any of
/// its bytes stay inside the mapping.
fn map(path: &str) -> &'static [u8] {
    let file = std::fs::File::open(path).expect(path);
    let len = file.metadata().unwrap().len() as usize;
    unsafe {
        let p = mmap(std::ptr::null_mut(), len + 4096, 1, 0x22, -1, 0); // PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS
        let ok = len == 0 || mmap(p, len, 1, 0x12, file.as_raw_fd(), 0) == p; // MAP_PRIVATE | MAP_FIXED
        assert!(p as isize != -1 && ok, "mmap {path}");
        std::slice::from_raw_parts(p, len)
    }
}

/// Runs f(t) on every thread and collects what each returns.
fn on_threads<R: Send>(f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    let f = &f;
    std::thread::scope(|s| {
        let hs: Vec<_> = (0..THREADS).map(|t| s.spawn(move || f(t))).collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    })
}

/// The decimal number at text[i..].
fn number(text: &[u8], i: usize) -> u32 {
    text[i..].iter().take_while(|b| b.is_ascii_digit()).fold(0, |v, &b| v * 10 + (b - b'0') as u32)
}

/// Token bytes: [A-Za-z0-9].
static WORD: [bool; 256] = {
    let mut t = [false; 256];
    let mut c = 0;
    while c < 256 {
        (t[c], c) = ((c as u8).is_ascii_alphanumeric(), c + 1);
    }
    t
};

/// The key of the token of len (at most 32) bytes at p: its bytes lowercased
/// (OR-ing 0x20 lowercases letters and keeps digits) and zero padded.
unsafe fn key(p: *const u8, len: usize) -> __m256i {
    static KEEP: [u8; 64] = {
        let (mut k, mut i) = ([0; 64], 0);
        while i < 32 {
            (k[i], i) = (0xFF, i + 1);
        }
        k
    };
    let v = _mm256_or_si256(_mm256_loadu_si256(p.cast()), _mm256_set1_epi8(0x20));
    _mm256_and_si256(v, _mm256_loadu_si256(KEEP.as_ptr().add(32 - len).cast()))
}

fn hash(k: __m256i) -> usize {
    let w = |a: i64, b: i64, m: u64| (a as u64 ^ (b as u64).rotate_left(29)).wrapping_mul(m);
    let h = unsafe {
        w(_mm256_extract_epi64::<0>(k), _mm256_extract_epi64::<2>(k), 0x9E3779B97F4A7C15)
            ^ w(_mm256_extract_epi64::<1>(k), _mm256_extract_epi64::<3>(k), 0xBF58476D1CE4E5B9)
    };
    (h ^ h >> 29) as usize
}

fn same(a: __m256i, b: __m256i) -> bool {
    unsafe { _mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)) == -1 }
}

/// Query terms to dense ids: open addressing, each slot the top 32 hash bits
/// above id + 1 (0 if free), so a mismatch rarely loads a key.
struct Terms {
    slot: Vec<u64>,
    keys: Vec<__m256i>,
}

impl Terms {
    fn find(&self, k: __m256i, h: usize) -> u32 {
        let mask = self.slot.len() - 1;
        let mut i = h & mask;
        loop {
            match self.slot[i] {
                0 => return NONE,
                s if s >> 32 == h as u64 >> 32 && same(self.keys[s as u32 as usize - 1], k) => return s as u32 - 1,
                _ => i = (i + 1) & mask,
            }
        }
    }

    /// Term i becomes new[i].
    fn renumber(&mut self, new: &[u32]) {
        let mut keys = self.keys.clone();
        new.iter().zip(&self.keys).for_each(|(&n, &k)| keys[n as usize] = k);
        self.keys = keys;
        self.slot.iter_mut().filter(|s| **s != 0).for_each(|s| *s = *s >> 32 << 32 | (new[*s as u32 as usize - 1] + 1) as u64);
    }

    fn insert(&mut self, term: &[u8]) -> u32 {
        let k = unsafe { key(term.as_ptr(), term.len()) };
        let (h, mask) = (hash(k), self.slot.len() - 1);
        let mut i = h & mask;
        while self.slot[i] != 0 {
            let s = self.slot[i];
            if s >> 32 == h as u64 >> 32 && same(self.keys[s as u32 as usize - 1], k) {
                return s as u32 - 1;
            }
            i = (i + 1) & mask;
        }
        self.keys.push(k);
        self.slot[i] = (h as u64 >> 32) << 32 | self.keys.len() as u64;
        self.keys.len() as u32 - 1
    }
}

/// Bits of the 64 bytes at p: token bytes ([A-Za-z0-9]; after OR-ing 0x20 a
/// letter lies in 'a'..='z', and a digit lies in '0'..='9' as is), and LFs.
unsafe fn classes(p: *const u8) -> (u64, u64) {
    let half = |q: *const u8| {
        let v = _mm256_loadu_si256(q.cast());
        let span = |x: __m256i, lo: u8, n: i8| _mm256_cmpeq_epi8(_mm256_subs_epu8(_mm256_sub_epi8(x, _mm256_set1_epi8(lo as i8)), _mm256_set1_epi8(n)), _mm256_setzero_si256());
        let word = _mm256_or_si256(span(_mm256_or_si256(v, _mm256_set1_epi8(0x20)), b'a', 25), span(v, b'0', 9));
        let nl = _mm256_cmpeq_epi8(v, _mm256_set1_epi8(b'\n' as i8));
        (_mm256_movemask_epi8(word) as u32 as u64, _mm256_movemask_epi8(nl) as u32 as u64)
    };
    let ((w0, n0), (w1, n1)) = (half(p), half(p.add(32)));
    (w0 | w1 << 32, n0 | n1 << 32)
}

/// Calls f(id, start, len) for each token of the document text from p on that
/// is a term, and returns where the next line starts. Token bounds come from
/// one class mask per 64-byte window; a token that runs into the window's end
/// starts the next window. Each token's slot is prefetched and the token
/// looked up LAG tokens later, so the misses overlap.
unsafe fn tokens(mut p: *const u8, terms: &Terms, mut f: impl FnMut(u32, *const u8, usize)) -> *const u8 {
    const LAG: usize = 8;
    let mut ring = [(_mm256_setzero_si256(), 0, p, 0); LAG];
    let mut look = |r: &(__m256i, usize, *const u8, usize)| match terms.find(r.0, r.1) {
        NONE => {}
        id => f(id, r.2, r.3),
    };
    let mut n = 0usize;
    loop {
        let (word, nl) = classes(p);
        let stop = if nl != 0 { nl.trailing_zeros() } else { 64 }; // the window's bytes in this document
        let mut m = word & ((1u128 << stop) - 1) as u64;
        let mut next = 64;
        while m != 0 {
            let s = m.trailing_zeros();
            let len = (!(m >> s)).trailing_zeros();
            if s + len == 64 && nl == 0 {
                next = s; // the token may go on: start the next window at it
                if s == 0 {
                    while WORD[*p.add(next as usize) as usize] {
                        next += 1; // 64 bytes or more: never a term
                    }
                }
                break;
            }
            m &= !0u64 << (s + len); // here the token ends inside the window
            if len <= 32 {
                let k = key(p.add(s as usize), len as usize);
                let h = hash(k);
                _mm_prefetch::<_MM_HINT_T0>(terms.slot.as_ptr().add(h & (terms.slot.len() - 1)).cast());
                if n >= LAG {
                    look(&ring[n % LAG]);
                }
                (ring[n % LAG], n) = ((k, h, p.add(s as usize), len as usize), n + 1);
            }
        }
        if nl != 0 {
            (n.saturating_sub(LAG)..n).for_each(|i| look(&ring[i % LAG]));
            return p.add(stop as usize + 1);
        }
        p = p.add(next as usize);
    }
}

/// With phrase[j] at text [s, s + len), do the tokens before and after it
/// spell the rest of the phrase? The document's TAB and LF bound the search.
unsafe fn around(terms: &Terms, s: *const u8, len: usize, phrase: &[u32], j: usize) -> bool {
    let is = |p: *const u8, q: *const u8, id: u32| {
        let n = q.offset_from(p) as usize;
        n <= 32 && same(key(p, n), terms.keys[id as usize])
    };
    let mut q = s;
    for &id in phrase[..j].iter().rev() {
        while !WORD[*q.sub(1) as usize] {
            if *q.sub(1) == b'\t' {
                return false;
            }
            q = q.sub(1);
        }
        let end = q;
        while WORD[*q.sub(1) as usize] {
            q = q.sub(1);
        }
        if !is(q, end, id) {
            return false;
        }
    }
    q = s.add(len);
    for &id in &phrase[j + 1..] {
        while !WORD[*q as usize] {
            if *q == b'\n' {
                return false;
            }
            q = q.add(1);
        }
        let start = q;
        while WORD[*q as usize] {
            q = q.add(1);
        }
        if !is(start, q, id) {
            return false;
        }
    }
    true
}

/// A set of documents: a sorted list, or a bitmap.
enum Docs {
    List(Vec<u32>),
    Bits(Vec<u64>),
}

fn has(bits: &[u64], d: u32) -> bool {
    bits[d as usize / 64] >> (d % 64) & 1 != 0
}

/// cur op term, strictly left to right; a list OR a bitmap becomes a bitmap.
fn apply(cur: Docs, op: u8, term: Docs) -> Docs {
    use Docs::*;
    let bit = |d: u32| (d as usize / 64, 1u64 << (d % 64));
    match (cur, op, term) {
        (Bits(mut a), _, Bits(b)) => {
            for (x, y) in a.iter_mut().zip(b) {
                *x = match op {
                    b'A' => *x & y,
                    b'O' => *x | y,
                    _ => *x & !y,
                };
            }
            Bits(a)
        }
        (Bits(a), b'A', List(b)) | (List(b), b'A', Bits(a)) => List(b.into_iter().filter(|&d| has(&a, d)).collect()),
        (List(a), _, Bits(b)) if op == b'N' => List(a.into_iter().filter(|&d| !has(&b, d)).collect()),
        (Bits(mut a), _, List(b)) | (List(b), _, Bits(mut a)) => {
            for (w, m) in b.into_iter().map(bit) {
                a[w] = if op == b'O' { a[w] | m } else { a[w] & !m };
            }
            Bits(a)
        }
        (List(a), b'O', List(b)) => {
            let (mut v, mut i, mut j) = (Vec::with_capacity(a.len() + b.len()), 0, 0);
            while i < a.len() && j < b.len() {
                let (x, y) = (a[i], b[j]);
                v.push(x.min(y));
                (i, j) = (i + (x <= y) as usize, j + (y <= x) as usize);
            }
            v.extend_from_slice(&a[i..]);
            v.extend_from_slice(&b[j..]);
            List(v)
        }
        // AND or NOT: a binary search per document when b is far longer, a merge otherwise.
        (List(mut a), _, List(b)) => {
            let (mut j, search) = (0, a.len() * 16 < b.len());
            a.retain(|&d| {
                if search {
                    j += b[j..].partition_point(|&x| x < d);
                } else {
                    while j < b.len() && b[j] < d {
                        j += 1;
                    }
                }
                (j < b.len() && b[j] == d) == (op == b'A')
            });
            List(a)
        }
    }
}

/// "count sum id1 ... id10" for a set of documents.
fn line(out: &mut String, docs: &Docs) {
    let (mut count, mut sum, mut first) = (0u64, 0u64, Vec::with_capacity(10));
    match docs {
        Docs::List(v) => {
            (count, sum) = (v.len() as u64, v.iter().map(|&d| d as u64).sum());
            first.extend(v.iter().take(10));
        }
        Docs::Bits(b) => {
            // Bit k of a position adds 2^k for each set bit whose position has it.
            const SEL: [u64; 6] = [0xAAAA_AAAA_AAAA_AAAA, 0xCCCC_CCCC_CCCC_CCCC, 0xF0F0_F0F0_F0F0_F0F0, 0xFF00_FF00_FF00_FF00, 0xFFFF_0000_FFFF_0000, 0xFFFF_FFFF_0000_0000];
            for (w, &x) in b.iter().enumerate().filter(|(_, &x)| x != 0) {
                let c = x.count_ones() as u64;
                let within: u64 = SEL.iter().enumerate().map(|(k, m)| ((x & m).count_ones() as u64) << k).sum();
                (count, sum) = (count + c, sum + c * 64 * w as u64 + within);
                let mut m = x;
                while m != 0 && first.len() < 10 {
                    first.push(w as u32 * 64 + m.trailing_zeros());
                    m &= m - 1;
                }
            }
        }
    }
    write!(out, "{count} {sum}").unwrap();
    first.iter().for_each(|d| write!(out, " {d}").unwrap());
    out.push('\n');
}

/// Calls f(doc, id, start, len) for the term tokens of the lines in text,
/// the first being document first; returns the number of documents.
fn walk(text: &[u8], first: u32, terms: &Terms, mut f: impl FnMut(u32, u32, *const u8, usize)) -> u32 {
    let (mut p, end, mut doc) = (text.as_ptr(), text.as_ptr_range().end, first);
    while p < end {
        unsafe {
            while *p != b'\t' {
                p = p.add(1);
            }
            p = tokens(p, terms, |id, s, len| f(doc, id, s, len));
        }
        doc += 1;
    }
    doc - first
}

/// Scan output for a bucket of terms, in 64 KB chunks so that only the last
/// one is ever part empty. A unit is a term's offset in the bucket above the
/// gap since the bucket's previous document; a thread meets its documents in
/// order, so the gaps are small.
#[derive(Default)]
struct Units {
    chunks: Vec<Vec<u16>>,
    prev: u32,
}

impl Units {
    fn push(&mut self, x: u16) {
        if self.chunks.last().map_or(true, |c| c.len() == c.capacity()) {
            self.chunks.push(Vec::with_capacity(1 << 15));
        }
        self.chunks.last_mut().unwrap().push(x);
    }

    fn add(&mut self, offset: u32, doc: u32) {
        let gap = doc - std::mem::replace(&mut self.prev, doc);
        self.push((offset << 5 | gap.min(GAP)) as u16);
        if gap >= GAP {
            self.push(gap as u16);
        }
    }
}

/// One thread's posting lists: term id's documents are first + post[start[id]..start[id + 1]].
struct Lists {
    first: u32,
    start: Vec<u32>,
    post: Vec<u16>,
}

fn main() {
    unsafe { mallopt(-3, 1 << 16) }; // M_MMAP_THRESHOLD: freed buffers go back to the kernel
    let qtext = map("/data/queries.txt");
    let mut terms = Terms { slot: vec![0; (qtext.len() / 8).next_power_of_two()], keys: Vec::with_capacity(qtext.len() / 16) };
    let mut queries: Vec<(bool, Vec<u32>, Vec<u8>)> = vec![]; // (phrase?, term ids, operators)
    let mut in_bool = vec![false; qtext.len() / 2];
    for q in qtext.split(|&b| b == b'\n').filter(|q| !q.is_empty()) {
        let phrase = q[0] == b'P';
        let (mut ids, mut ops) = (vec![], vec![]);
        for (k, word) in q[if phrase { 8 } else { 5 }..q.len() - phrase as usize].split(|&b| b == b' ').enumerate() {
            if !phrase && k % 2 == 1 {
                ops.push(word[0]);
            } else {
                ids.push(terms.insert(word));
                in_bool[*ids.last().unwrap() as usize] |= !phrase;
            }
        }
        queries.push((phrase, ids, ops));
    }
    let nterms = terms.keys.len();

    let text = map("/data/docs.txt");
    let mut cut: Vec<usize> = (0..=THREADS).map(|t| text.len() * t / THREADS).collect();
    for c in &mut cut[1..THREADS] {
        while *c < text.len() && text[*c - 1] != b'\n' {
            *c += 1;
        }
    }
    let ndocs = match text.len() {
        0 => 0,
        n => number(text, text[..n - 1].iter().rposition(|&b| b == b'\n').map_or(0, |i| i + 1)) as usize + 1,
    };
    let firsts: Vec<u32> = (0..THREADS).map(|t| if cut[t] < text.len() { number(text, cut[t]) } else { ndocs as u32 }).collect();

    // The sample: documents per term in the first 1/32 of each share.
    let samples = on_threads(|t| {
        let (mut df, mut last) = (vec![0u32; nterms], vec![NONE; nterms]);
        let sample = &text[cut[t]..cut[t] + (cut[t + 1] - cut[t]) / 32];
        let n = walk(sample, firsts[t], &terms, |doc, id, _, _| {
            if std::mem::replace(&mut last[id as usize], doc) != doc {
                df[id as usize] += 1;
            }
        });
        (n as usize, df)
    });
    let sampled: usize = samples.iter().map(|s| s.0).sum();
    let mut df: Vec<u32> = (0..nterms).map(|i| samples.iter().map(|s| s.1[i]).sum()).collect();
    // Terms renumbered most frequent first, so the per-term state of the
    // common ones shares a few cache lines during the scan.
    let mut order: Vec<usize> = (0..nterms).collect();
    order.sort_by_key(|&i| std::cmp::Reverse(df[i]));
    let mut new = vec![0; nterms];
    order.iter().enumerate().for_each(|(r, &i)| new[i] = r as u32);
    terms.renumber(&new);
    queries.iter_mut().for_each(|q| q.1.iter_mut().for_each(|id| *id = new[*id as usize]));
    (df, in_bool) = (order.iter().map(|&i| df[i]).collect(), order.iter().map(|&i| in_bool[i]).collect());
    // Per term: NONE if no BOOL query needs it, 0 for a list, b + 1 for bitmap b.
    let mut nbits = 0usize;
    let role: Vec<u32> = (0..nterms)
        .map(|i| match (in_bool[i], df[i] as usize * 32 > sampled) {
            (false, _) => NONE,
            (true, false) => 0,
            (true, true) => (nbits, nbits += 1).0 as u32 + 1,
        })
        .collect();
    // Per term, the phrases anchored at it (query, position), from at[id].
    let mut anchored: Vec<(u32, u32, u32)> = queries.iter().enumerate().filter(|(_, q)| q.0).map(|(q, (_, ids, _))| {
        let j = (0..ids.len()).min_by_key(|&j| df[ids[j] as usize]).unwrap();
        (ids[j], q as u32, j as u32)
    }).collect();
    anchored.sort_unstable();
    let mut at = vec![0usize; nterms + 1];
    anchored.iter().for_each(|a| at[a.0 as usize + 1] += 1);
    (0..nterms).for_each(|i| at[i + 1] += at[i]);
    let words = ndocs.div_ceil(64);
    // Zero pages from the kernel, written only where a bit is set.
    let bits: Vec<AtomicU64> = unsafe { std::mem::transmute(vec![0u64; nbits * words]) };
    // Per term, its role and where its anchored phrases start, side by side.
    let info: Vec<(u32, u32)> = (0..=nterms).map(|i| (role.get(i).copied().unwrap_or(NONE), at[i] as u32)).collect();

    // The scan, then each thread's buckets counting-sorted into its lists.
    let found = Mutex::new(vec![]); // (query, document) phrase matches
    let lists: Vec<Lists> = on_threads(|t| {
        // Per term: documents in this share, and the last one's offset + 1.
        let (mut count, mut last, mut mine) = (vec![0u16; nterms], vec![0u16; nterms], vec![]);
        let mut buckets: Vec<Units> = (0..=nterms >> BUCKET).map(|_| Units::default()).collect();
        let first = firsts[t];
        // A bitmap term's bits for the current 64 documents: (word, bits).
        let mut pending = vec![(0usize, 0u64); nbits];
        let flush = |(w, x): (usize, u64), b: usize| {
            if x != 0 {
                bits[b * words + w].fetch_or(x, Relaxed);
            }
        };
        let n = walk(&text[cut[t]..cut[t + 1]], first, &terms, |doc, id, s, len| {
            let i = id as usize;
            let ((role, from), (_, to)) = (info[i], info[i + 1]);
            for &(_, q, j) in &anchored[from as usize..to as usize] {
                if unsafe { around(&terms, s, len, &queries[q as usize].1, j as usize) } {
                    mine.push((q, doc));
                }
            }
            if role == NONE || std::mem::replace(&mut last[i], (doc - first + 1) as u16) == (doc - first + 1) as u16 {
                return;
            }
            match role {
                0 => {
                    buckets[i >> BUCKET].add((i & ((1 << BUCKET) - 1)) as u32, doc - first);
                    count[i] += 1;
                }
                b => {
                    let (b, w) = ((b - 1) as usize, doc as usize / 64);
                    if pending[b].0 != w {
                        flush(std::mem::replace(&mut pending[b], (w, 0)), b);
                    }
                    pending[b].1 |= 1 << (doc % 64);
                }
            }
        });
        pending.into_iter().enumerate().for_each(|(b, p)| flush(p, b));
        assert!(n < 1 << DOC_BITS, "too many documents per thread");
        found.lock().unwrap().extend(mine);
        let mut start = vec![0u32; nterms + 1];
        (0..nterms).for_each(|i| start[i + 1] = start[i] + count[i] as u32);
        drop((count, last));
        let mut post = vec![0u16; start[nterms] as usize];
        let mut cursor = start.clone();
        for (b, units) in buckets.into_iter().enumerate() {
            let (mut doc, mut term) = (0u32, NONE);
            for x in units.chunks.into_iter().flatten() {
                if term == NONE {
                    term = (b << BUCKET) as u32 | (x >> 5) as u32;
                    if x as u32 & GAP == GAP {
                        continue; // the whole gap follows
                    }
                    doc += x as u32 & GAP;
                } else {
                    doc += x as u32;
                }
                let i = std::mem::replace(&mut term, NONE) as usize;
                (post[cursor[i] as usize], cursor[i]) = (doc as u16, cursor[i] + 1);
            }
        }
        Lists { first, start, post }
    });
    let bits: Vec<u64> = bits.into_iter().map(AtomicU64::into_inner).collect();
    let mut found = found.into_inner().unwrap();
    found.sort_unstable();
    found.dedup();

    // Answers, 64 queries at a time on every thread. Large lists come and go
    // here, so they now reuse heap memory instead of a fresh mapping each.
    unsafe { mallopt(-3, 64 << 20) };
    let docs = |id: u32| match role[id as usize] {
        0 => {
            let range = |l: &Lists| l.start[id as usize] as usize..l.start[id as usize + 1] as usize;
            let mut v = Vec::with_capacity(lists.iter().map(|l| range(l).len()).sum());
            lists.iter().for_each(|l| v.extend(l.post[range(l)].iter().map(|&d| l.first + d as u32)));
            Docs::List(v)
        }
        b => Docs::Bits(bits[(b - 1) as usize * words..b as usize * words].to_vec()),
    };
    let chunks: Vec<Mutex<String>> = (0..queries.len().div_ceil(64)).map(|_| Mutex::default()).collect();
    let next = AtomicUsize::new(0);
    on_threads(|_| {
        loop {
            let k = next.fetch_add(1, Relaxed);
            let Some(chunk) = chunks.get(k) else { return };
            let mut out = String::new();
            for (q, (phrase, ids, ops)) in queries.iter().enumerate().skip(k * 64).take(64) {
                let set = if *phrase {
                    let from = found.partition_point(|m| m.0 < q as u32);
                    Docs::List(found[from..].iter().take_while(|m| m.0 == q as u32).map(|m| m.1).collect())
                } else {
                    ops.iter().zip(&ids[1..]).fold(docs(ids[0]), |cur, (&op, &id)| apply(cur, op, docs(id)))
                };
                line(&mut out, &set);
            }
            *chunk.lock().unwrap() = out;
        }
    });
    let out: String = chunks.into_iter().map(|c| c.into_inner().unwrap()).collect();
    std::fs::write("/out/results.txt", out).expect("/out/results.txt");
}
