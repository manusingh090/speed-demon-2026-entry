//! Speed Demon 2026, Track B (Index), in Rust with no dependencies.
//!
//! 1. The queries are read first, and only their terms are looked for.
//! 2. Each thread samples the start of its share of docs.txt, counting the
//!    documents that hold each term: a BOOL term in over 1/32 of them is kept
//!    as a bitmap, and each phrase's rarest term becomes its anchor.
//! 3. The full scan. Every phrase is checked wherever its anchor occurs, while
//!    the text is in cache. A BOOL term in a document sets its bitmap bit or
//!    files the pair under the term's bucket; the buckets are then scattered
//!    into one stretch of 16-bit doc offsets per term.
//! 4. BOOL queries run left to right on sorted lists, or on a bitmap once a
//!    bitmap term starts the query or is OR-ed in.
use std::arch::x86_64::*;
use std::borrow::Cow;
use std::fmt::Write;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU16, AtomicU64, AtomicUsize, Ordering::Relaxed};
use std::sync::Mutex;

extern "C" {
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    fn mallopt(param: i32, value: i32) -> i32;
    fn prctl(option: i32, ...) -> i32;
}

const THREADS: usize = 12;
const NONE: u32 = u32::MAX;
const BUCKETS: usize = 32; // scan output streams per thread; term i goes to bucket i % BUCKETS
const GAP: u32 = 15; // a unit's 4-bit doc gap; GAP means the whole gap follows in the next unit
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
    let digits = text[i..].iter().take_while(|b| b.is_ascii_digit());
    digits.fold(0, |v, &b| v * 10 + (b - b'0') as u32)
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
    let w: [u64; 4] = unsafe { std::mem::transmute(k) };
    let h = (w[0] ^ w[2].rotate_left(29)).wrapping_mul(0x9E3779B97F4A7C15);
    let h = h ^ (w[1] ^ w[3].rotate_left(29)).wrapping_mul(0xBF58476D1CE4E5B9);
    (h ^ h >> 29) as usize
}

fn same(a: __m256i, b: __m256i) -> bool {
    unsafe { _mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)) == -1 }
}

/// Query terms to dense ids: open addressing, each slot the top 32 hash bits
/// above id + 1 (0 if free), so a mismatch rarely loads a key. A token that
/// is no term gets the id one past the last term, so callers need no branch.
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
                0 => return self.keys.len() as u32,
                s if s >> 32 == h as u64 >> 32 && same(self.keys[s as u32 as usize - 1], k) => return s as u32 - 1,
                _ => i = (i + 1) & mask,
            }
        }
    }

    /// Term i becomes new[i].
    fn renumber(&mut self, new: &[u32]) {
        let mut keys = self.keys.clone();
        for (&n, &k) in new.iter().zip(&self.keys) {
            keys[n as usize] = k;
        }
        self.keys = keys;
        for s in self.slot.iter_mut().filter(|s| **s != 0) {
            *s = *s >> 32 << 32 | (new[*s as u32 as usize - 1] + 1) as u64;
        }
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
        let (v, c) = (_mm256_loadu_si256(q.cast()), |b: u8| _mm256_set1_epi8(b as i8));
        let at_most = |x: __m256i, n: u8| _mm256_cmpeq_epi8(_mm256_subs_epu8(x, c(n)), c(0));
        let letter = _mm256_sub_epi8(_mm256_or_si256(v, c(0x20)), c(b'a'));
        let word = _mm256_or_si256(at_most(letter, 25), at_most(_mm256_sub_epi8(v, c(b'0')), 9));
        let mask = |x: __m256i| _mm256_movemask_epi8(x) as u32 as u64;
        (mask(word), mask(_mm256_cmpeq_epi8(v, c(b'\n'))))
    };
    let ((w0, n0), (w1, n1)) = (half(p), half(p.add(32)));
    (w0 | w1 << 32, n0 | n1 << 32)
}

/// Calls f(id, start, len) for each token of the document text from p on that
/// is a term, and returns where the next line starts. Token bounds come from
/// one class mask per 64-byte window; a token that runs into the window's end
/// starts the next window. Each token's slot is prefetched and the token
/// looked up LAG tokens later, so the misses overlap.
unsafe fn tokens(mut p: *const u8, t: &Terms, mut f: impl FnMut(u32, *const u8, usize)) -> *const u8 {
    const LAG: usize = 8;
    let mut ring = [(_mm256_setzero_si256(), 0, p, 0); LAG];
    let mut look = |r: &(__m256i, usize, *const u8, usize)| f(t.find(r.0, r.1), r.2, r.3);
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
                let slot = t.slot.as_ptr().add(h & (t.slot.len() - 1));
                _mm_prefetch::<_MM_HINT_T0>(slot.cast());
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
#[inline(never)] // rare (anchor tokens only); inlined it keeps the per-token code out of line
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

/// A set of documents: a sorted list, or a bitmap. A term's bitmap is
/// borrowed, and copied only when an operator first writes to it.
enum Docs<'a> {
    List(Vec<u32>),
    Bits(Cow<'a, [u64]>),
}

fn has(bits: &[u64], d: u32) -> bool {
    bits[d as usize / 64] >> (d % 64) & 1 != 0
}

/// cur op term, strictly left to right; a list OR a bitmap becomes a bitmap.
fn apply<'a>(cur: Docs<'a>, op: u8, term: Docs<'a>) -> Docs<'a> {
    use Docs::*;
    let bit = |d: u32| (d as usize / 64, 1u64 << (d % 64));
    match (cur, op, term) {
        (Bits(mut a), _, Bits(b)) => {
            for (x, &y) in a.to_mut().iter_mut().zip(b.iter()) {
                *x = match op {
                    b'A' => *x & y,
                    b'O' => *x | y,
                    _ => *x & !y,
                };
            }
            Bits(a)
        }
        (Bits(a), b'A', List(mut b)) | (List(mut b), b'A', Bits(a)) => (b.retain(|&d| has(&a, d)), List(b)).1,
        (List(mut a), _, Bits(b)) if op == b'N' => (a.retain(|&d| !has(&b, d)), List(a)).1,
        (Bits(mut a), _, List(b)) | (List(b), _, Bits(mut a)) => {
            let x = a.to_mut();
            for (w, m) in b.into_iter().map(bit) {
                x[w] = if op == b'O' { x[w] | m } else { x[w] & !m };
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
        // AND or NOT: a binary search per document when b is far longer.
        (List(mut a), _, List(b)) if a.len() * 16 < b.len() => {
            let mut j = 0;
            a.retain(|&d| {
                j += b[j..].partition_point(|&x| x < d);
                (j < b.len() && b[j] == d) == (op == b'A')
            });
            List(a)
        }
        // Otherwise a merge without branches: x stays when it is in b (AND)
        // or below b's next document (NOT), and a's rest stays for NOT.
        (List(mut a), _, List(b)) => {
            let (mut i, mut j, mut k) = (0, 0, 0);
            while i < a.len() && j < b.len() {
                let (x, y) = (a[i], b[j]);
                a[k] = x;
                k += if op == b'A' { x == y } else { x < y } as usize;
                (i, j) = (i + (x <= y) as usize, j + (y <= x) as usize);
            }
            let rest = if op == b'A' { 0 } else { a.len() - i };
            a.copy_within(i..i + rest, k);
            a.truncate(k + rest);
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
                let within: u64 = (0..6).map(|k| ((x & SEL[k]).count_ones() as u64) << k).sum();
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
fn walk(text: &[u8], first: u32, t: &Terms, mut f: impl FnMut(u32, u32, *const u8, usize)) -> u32 {
    let (mut p, end, mut doc) = (text.as_ptr(), text.as_ptr_range().end, first);
    while p < end {
        unsafe {
            while *p != b'\t' {
                p = p.add(1);
            }
            p = tokens(p, t, #[inline(always)] |id, s, len| f(doc, id, s, len));
        }
        doc += 1;
    }
    doc - first
}

/// Scan output for a bucket of terms, in 64 KB chunks so that only the last
/// one is ever part empty. A unit is a term's index in the bucket (i / BUCKETS)
/// above the gap since the bucket's previous document; a thread meets its
/// documents in order, so the gaps are small.
#[derive(Default)]
struct Units {
    chunks: Vec<Vec<u16>>,
    at: usize, // the address of the next unit's place in the last chunk, up to end
    end: usize,
    prev: u32,
}

impl Units {
    /// Writes x at the next place, but moves past it only if keep, so a token
    /// that adds nothing costs a store and no branch.
    fn put(&mut self, x: u16, keep: bool) {
        if self.at == self.end {
            self.grow();
        }
        unsafe { *(self.at as *mut u16) = x };
        self.at += 2 * keep as usize;
    }

    #[cold]
    #[inline(never)]
    fn grow(&mut self) {
        self.finish();
        let c = Vec::<u16>::with_capacity(1 << 15);
        (self.at, self.end) = (c.as_ptr() as usize, c.as_ptr() as usize + 2 * c.capacity());
        self.chunks.push(c);
    }

    fn add(&mut self, keep: bool, offset: u32, doc: u32) {
        let gap = doc - self.prev;
        self.put((offset << 4 | gap.min(GAP)) as u16, keep);
        self.prev = if keep { doc } else { self.prev };
        if keep & (gap >= GAP) {
            self.put(gap as u16, true);
        }
    }

    /// Sets the last chunk's length to the units written.
    fn finish(&mut self) {
        if let Some(c) = self.chunks.last_mut() {
            unsafe { c.set_len((self.at - c.as_ptr() as usize) / 2) };
        }
    }
}


fn main() {
    unsafe { mallopt(-3, 1 << 16) }; // M_MMAP_THRESHOLD: freed buffers go back to the kernel
    // PR_SET_THP_DISABLE: the box backs part-written regions with whole 2 MB pages.
    unsafe { prctl(41, 1u64, 0u64, 0u64, 0u64) };
    let qtext = map("/data/queries.txt");
    let slots = vec![0; (qtext.len() / 8).next_power_of_two()];
    let mut terms = Terms { slot: slots, keys: Vec::with_capacity(qtext.len() / 16) };
    // Queries, flat: query q's terms are ids[span.1..span.2], and for BOOL
    // ops[i] joins ids[i] and ids[i + 1].
    let (mut spans, mut ids, mut ops) = (vec![], vec![], vec![]);
    for q in qtext.split(|&b| b == b'\n').filter(|q| !q.is_empty()) {
        let (phrase, from) = (q[0] == b'P', ids.len() as u32);
        let body = &q[if phrase { 8 } else { 5 }..q.len() - phrase as usize];
        for (k, word) in body.split(|&b| b == b' ').enumerate() {
            match !phrase && k % 2 == 1 {
                true => *ops.last_mut().unwrap() = word[0],
                false => (ids.push(terms.insert(word)), ops.push(0)).1,
            }
        }
        spans.push((phrase, from, ids.len() as u32));
    }
    let nterms = terms.keys.len();
    assert!(nterms <= BUCKETS << 12, "too many terms"); // a unit's index has 12 bits
    let mut in_bool = vec![false; nterms];
    for &(phrase, from, to) in spans.iter().filter(|s| !s.0) {
        ids[from as usize..to as usize].iter().for_each(|&id| in_bool[id as usize] = !phrase);
    }

    let text = map("/data/docs.txt");
    let mut cut: Vec<usize> = (0..=THREADS).map(|t| text.len() * t / THREADS).collect();
    for c in &mut cut[1..THREADS] {
        while *c < text.len() && text[*c - 1] != b'\n' {
            *c += 1;
        }
    }
    let mut last_line = text.len().saturating_sub(1);
    while last_line > 0 && text[last_line - 1] != b'\n' {
        last_line -= 1;
    }
    let ndocs = if text.is_empty() { 0 } else { number(text, last_line) as usize + 1 };
    let first_doc = |c: usize| text.get(c).map_or(ndocs as u32, |_| number(text, c));
    let firsts: Vec<u32> = cut[..THREADS].iter().map(|&c| first_doc(c)).collect();

    // The sample: documents per term in the first 1/32 of each share.
    let samples = on_threads(|t| {
        let (mut df, mut last) = (vec![0u32; nterms + 1], vec![NONE; nterms + 1]);
        let sample = &text[cut[t]..cut[t] + (cut[t + 1] - cut[t]) / 32];
        let n = walk(sample, firsts[t], &terms, |doc, id, _, _| {
            if std::mem::replace(&mut last[id as usize], doc) != doc {
                df[id as usize] += 1;
            }
        });
        (n as usize, df)
    });
    let sampled: usize = samples.iter().map(|s| s.0).sum();
    let mut df = vec![0u32; nterms + 1];
    for (_, d) in samples {
        df.iter_mut().zip(d).for_each(|(a, b)| *a += b);
    }
    // Each phrase's anchor, its rarest term: (anchor, query, position).
    let (mut anchored, mut anchor) = (vec![], vec![false; nterms]);
    for (q, &(_, from, to)) in spans.iter().enumerate().filter(|s| s.1 .0) {
        let j = (from..to).min_by_key(|&j| df[ids[j as usize] as usize]).unwrap();
        anchored.push((ids[j as usize], q as u32, j - from));
        anchor[ids[j as usize] as usize] = true;
    }
    // Terms renumbered by class, so the scan reads a token's role off its id:
    // bitmaps, lists, lists that anchor phrases, anchors only, and terms only
    // phrases read (their tokens need no state). Within a class the most
    // frequent come first, so the state of common terms shares cache lines.
    let class = |i: usize| match (in_bool[i], anchor[i]) {
        (true, false) => (df[i] as usize * 32 <= sampled) as usize,
        (true, true) => 2,
        (false, true) => 3,
        (false, false) => 4,
    };
    let mut order: Vec<usize> = (0..nterms).collect();
    order.sort_by_key(|&i| (class(i), std::cmp::Reverse(df[i])));
    let [nbits, a0, lists, nkeep] = [0, 1, 2, 3].map(|c| order.partition_point(|&i| class(i) <= c));
    let mut new = vec![0; nterms];
    for (r, &i) in order.iter().enumerate() {
        new[i] = r as u32;
    }
    terms.renumber(&new);
    ids.iter_mut().for_each(|id| *id = new[*id as usize]);
    anchored.iter_mut().for_each(|a| a.0 = new[a.0 as usize]);
    anchored.sort_unstable();
    // Where the phrases of anchor a0 + k start in anchored.
    let astart: Vec<usize> = (a0..=nkeep).map(|i| anchored.partition_point(|a| (a.0 as usize) < i)).collect();
    let words = ndocs.div_ceil(64);
    // Zero pages from the kernel, written only where a bit is set.
    let bits: Vec<AtomicU64> = unsafe { std::mem::transmute(vec![0u64; nbits * words]) };

    // The scan: phrase matches, bitmap bits, and list postings by bucket.
    let found = Mutex::new(vec![]); // (query, document) phrase matches
    let scans: Vec<(Vec<u16>, Mutex<Vec<Units>>)> = on_threads(|t| {
        // Per term (and one id for every token that needs nothing): documents
        // in this share, and the last one's offset + 1.
        let (mut count, mut last, mut mine) = (vec![0u16; nkeep + 1], vec![0u16; nkeep + 1], vec![]);
        let mut buckets: Vec<Units> = (0..BUCKETS).map(|_| Units::default()).collect();
        let first = firsts[t];
        // Each bitmap term's bits for the current 64 documents (one more word
        // takes the bits of every other term), ORed in when the block ends.
        let (mut pending, mut block) = (vec![0u64; nbits + 1], first as usize / 64);
        let flush = |pending: &mut [u64], w: usize| {
            for (b, x) in pending[..nbits].iter_mut().enumerate().filter(|(_, x)| **x != 0) {
                bits[b * words + w].fetch_or(std::mem::take(x), Relaxed);
            }
        };
        // Every token does the same work, kept or not by masks, so whether a
        // term is new to the document, a list or a bitmap costs no branch.
        let n = walk(&text[cut[t]..cut[t + 1]], first, &terms, #[inline(always)] |doc, id, s, len| {
            let i = (id as usize).min(nkeep);
            if i.wrapping_sub(a0) < nkeep - a0 {
                let k = i - a0;
                for &(_, q, j) in &anchored[astart[k]..astart[k + 1]] {
                    let span = spans[q as usize];
                    let phrase = &ids[span.1 as usize..span.2 as usize];
                    if unsafe { around(&terms, s, len, phrase, j as usize) } {
                        mine.push((q, doc));
                    }
                }
            }
            if doc as usize / 64 != block {
                let w = std::mem::replace(&mut block, doc as usize / 64);
                flush(&mut pending, w);
            }
            let mark = (doc - first + 1) as u16;
            let new = std::mem::replace(&mut last[i], mark) != mark;
            let list = new & (i.wrapping_sub(nbits) < lists - nbits);
            pending[i.min(nbits)] |= ((new & (i < nbits)) as u64) << (doc % 64);
            buckets[i % BUCKETS].add(list, (i / BUCKETS) as u32, doc - first);
            count[i] += list as u16;
        });
        flush(&mut pending, block);
        buckets.iter_mut().for_each(Units::finish);
        assert!(n < 1 << DOC_BITS, "too many documents per thread");
        found.lock().unwrap().extend(mine);
        (count, Mutex::new(buckets))
    });
    // Each term's postings lie in one stretch, thread by thread: the segment
    // from thread t holds seg[i * THREADS + t] offsets from firsts[t]. The
    // stretches go bucket by bucket, so a bucket's scatter writes one region,
    // and frequency ranks dealt round-robin keep the regions small.
    let (mut seg, mut start, mut end) = (vec![0u16; lists * THREADS], vec![0u32; lists], 0);
    for i in (0..BUCKETS).flat_map(|b| (b..lists).step_by(BUCKETS)) {
        let s = &mut seg[i * THREADS..][..THREADS];
        s.iter_mut().zip(&scans).for_each(|(n, scan)| *n = scan.0[i]);
        (start[i], end) = (end, end + s.iter().map(|&n| n as u32).sum::<u32>());
    }
    // Written by every thread at its own positions; relaxed stores are plain stores.
    let post: Vec<AtomicU16> = unsafe { std::mem::transmute(vec![0u16; end as usize]) };
    on_threads(|t| {
        let buckets = std::mem::take(&mut *scans[t].1.lock().unwrap());
        let mut cursor = vec![0u32; lists.div_ceil(BUCKETS)];
        for (b, units) in buckets.into_iter().enumerate() {
            for (c, i) in cursor.iter_mut().zip((b..lists).step_by(BUCKETS)) {
                *c = start[i] + seg[i * THREADS..][..t].iter().map(|&n| n as u32).sum::<u32>();
            }
            let (mut doc, mut term) = (0u32, NONE);
            for x in units.chunks.into_iter().flatten() {
                if term == NONE {
                    term = (x >> 4) as u32;
                    if x as u32 & GAP == GAP {
                        continue; // the whole gap follows
                    }
                    doc += x as u32 & GAP;
                } else {
                    doc += x as u32;
                }
                let c = &mut cursor[std::mem::replace(&mut term, NONE) as usize];
                (post[*c as usize].store(doc as u16, Relaxed), *c += 1);
            }
        }
    });
    drop(scans);
    let post: Vec<u16> = unsafe { std::mem::transmute(post) };
    let bits: Vec<u64> = unsafe { std::mem::transmute(bits) };
    let mut found = found.into_inner().unwrap();
    found.sort_unstable();
    found.dedup();

    // Answers, 64 queries at a time on every thread. Large lists come and go
    // here, so they now reuse heap memory instead of a fresh mapping each.
    unsafe { mallopt(-3, 64 << 20) };
    let docs = |id: u32| match id as usize {
        i if i < nbits => Docs::Bits(Cow::Borrowed(&bits[i * words..(i + 1) * words])),
        i => {
            let mut at = start[i] as usize;
            let segs = &seg[i * THREADS..][..THREADS];
            let mut v = Vec::with_capacity(segs.iter().map(|&n| n as usize).sum());
            for (&n, &first) in segs.iter().zip(&firsts) {
                v.extend(post[at..at + n as usize].iter().map(|&d| first + d as u32));
                at += n as usize;
            }
            Docs::List(v)
        }
    };
    let chunks: Vec<Mutex<String>> = (0..spans.len().div_ceil(64)).map(|_| Mutex::default()).collect();
    let next = AtomicUsize::new(0);
    on_threads(|_| loop {
        let k = next.fetch_add(1, Relaxed);
        let Some(chunk) = chunks.get(k) else { return };
        let mut out = String::new();
        for (q, &(phrase, from, to)) in spans.iter().enumerate().skip(k * 64).take(64) {
            let (tids, ops) = (&ids[from as usize..to as usize], &ops[from as usize..to as usize]);
            let set = if phrase {
                let at = found.partition_point(|m| m.0 < q as u32);
                Docs::List(found[at..].iter().take_while(|m| m.0 == q as u32).map(|m| m.1).collect())
            } else {
                let cur = docs(tids[0]);
                ops.iter().zip(&tids[1..]).fold(cur, |cur, (&op, &id)| match &cur {
                    Docs::List(v) if v.is_empty() && op != b'O' => cur, // nothing left to narrow
                    _ => apply(cur, op, docs(id)),
                })
            };
            line(&mut out, &set);
        }
        *chunk.lock().unwrap() = out;
    });
    let out: String = chunks.into_iter().map(|c| c.into_inner().unwrap()).collect();
    std::fs::write("/out/results.txt", out).expect("/out/results.txt");
}
