//! Speed Demon 2026, Track B (Index), in Rust with no dependencies.
//!
//! 1. Parse the queries first, in parallel, into a table of every term they
//!    mention. Tokens outside that table are never indexed.
//! 2. Sample. docs.txt is memory-mapped and cut into one line-aligned range per
//!    thread; each thread counts terms over the first 1/32 of its range. From
//!    the counts: each phrase's rarest term becomes its anchor; BOOL terms in
//!    over 1/32 of documents become bitmaps and the other BOOL terms lists;
//!    terms are renumbered so the scan tells these apart by id alone.
//! 3. Scan. A token's table slot is prefetched and looked up 8 tokens later.
//!    At an anchor, the phrases it anchors are checked in place, while the text
//!    is in cache, and only the matches are kept. Bitmap terms set a bit; every
//!    other (term, document) pair is filed, as a 16-bit unit, under a bucket of
//!    2,048 term ids in blocks mapped from the kernel.
//! 4. Scatter each thread's buckets into its own region of posting lists, as
//!    16-bit offsets from its first document, unmapping blocks as they are read.
//! 5. Answer. A PHRASE's documents are the scan's matches. BOOL runs strictly
//!    left to right on sorted lists, galloping from the smaller side, and
//!    switches to a bitmap once a bitmap term starts the query or is OR-ed in.
use std::arch::x86_64::*;
use std::ffi::c_void;
use std::io::Write;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering::*};

extern "C" {
    fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> i32;
    fn prctl(op: i32, a: u64, b: u64, c: u64, d: u64) -> i32;
    fn mallopt(param: i32, value: i32) -> i32;
}

const NONE: u32 = u32::MAX;
const MAX_TERM: usize = 32; // query terms are 1 to 32 bytes
const LAG: usize = 8; // tokens between a slot's prefetch and its lookup
// Scan output: a unit is a term's offset in its bucket above the gap since the
// bucket's previous document; a gap of GAP or more is GAP plus a whole unit.
const BUCKET_BITS: u32 = 11;
const GAP_BITS: u32 = 16 - BUCKET_BITS;
const GAP: u32 = (1 << GAP_BITS) - 1;
const DOC_BITS: u32 = 15; // documents per thread fit in 15 bits
const BLOCK: usize = 1 << 15; // 16-bit units per block of scan output (64 KB)
const CHUNK: usize = 64; // queries handed to a thread at a time

/// A pointer the threads share; every use writes indices no other thread writes.
#[derive(Clone, Copy)]
struct Raw<T>(*mut T);
unsafe impl<T> Send for Raw<T> {}
unsafe impl<T> Sync for Raw<T> {}
impl<T> Raw<T> {
    fn of(v: &mut [T]) -> Self {
        Raw(v.as_mut_ptr())
    }
    unsafe fn at(self, i: usize) -> *mut T {
        self.0.add(i)
    }
}

fn map(path: &str) -> &'static [u8] {
    let file = std::fs::File::open(path).expect(path);
    let len = file.metadata().unwrap().len() as usize;
    // An anonymous page of zeros, then the file over its start, so a 64-byte
    // read at any byte of the file stays inside the mapping.
    unsafe {
        let p = mmap(std::ptr::null_mut(), len + 4096, 1, 0x22, -1, 0);
        let ok = len == 0 || mmap(p, len, 1, 0x12, file.as_raw_fd(), 0) == p;
        assert!(p as isize != -1 && ok, "mmap {path}");
        std::slice::from_raw_parts(p as *const u8, len)
    }
}

/// k line-aligned cuts of text.
fn cuts(text: &[u8], k: usize) -> Vec<usize> {
    let mut c = vec![text.len(); k + 1];
    c[0] = 0;
    for t in 1..k {
        let mut x = (text.len() * t / k).max(c[t - 1]);
        while x > 0 && x < text.len() && text[x - 1] != b'\n' {
            x += 1;
        }
        c[t] = x;
    }
    c
}

/// Runs f(t) on nt threads and collects what each returns.
fn on_threads<R: Send>(nt: usize, f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    let f = &f;
    std::thread::scope(|s| {
        let hs: Vec<_> = (0..nt).map(|t| s.spawn(move || f(t))).collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    })
}

/// Index of the first byte c at or after text[from]; c must occur.
fn find_byte(text: &[u8], from: usize, c: u8) -> usize {
    unsafe {
        let want = _mm256_set1_epi8(c as i8);
        let mut i = from;
        loop {
            let v = _mm256_loadu_si256(text.as_ptr().add(i) as *const __m256i);
            let m = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, want)) as u32;
            if m != 0 {
                return i + m.trailing_zeros() as usize;
            }
            i += 32;
        }
    }
}

fn parse_id(text: &[u8]) -> u32 {
    text.iter().take_while(|c| c.is_ascii_digit()).fold(0, |v, &c| v * 10 + (c - b'0') as u32)
}

/// A term key: up to 32 lowercase bytes, zero padded (no term holds a zero
/// byte, so the padding encodes the length).
type Key = [u64; 4];

/// The key of the token at s, len bytes long. Token bytes are [A-Za-z0-9], so
/// OR-ing 0x20 lowercases letters and leaves digits alone.
#[inline(always)]
fn load_key(s: *const u8, len: usize) -> Key {
    let mut k = [0u64; 4];
    for (i, w) in k.iter_mut().enumerate() {
        let b = len.saturating_sub(8 * i).min(8);
        let x = unsafe { (s.add(8 * i) as *const u64).read_unaligned() };
        *w = (x | 0x2020_2020_2020_2020) & if b == 8 { !0 } else { (1u64 << (8 * b)) - 1 };
    }
    k
}

/// Two independent multiplies; almost every token fits in the first two words.
#[inline(always)]
fn hash_key(k: &Key) -> u64 {
    let h = k[0].wrapping_add(k[2].wrapping_mul(0x94D0_49BB_1331_11EB)).wrapping_mul(0x9E37_79B9_7F4A_7C15)
        ^ k[1].wrapping_add(k[3].wrapping_mul(0xD6E8_FEB8_6659_FD93)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    h ^ h >> 29
}

/// Open-addressing table from term key to a dense term id. A slot holds the
/// top 32 hash bits and id + 1 (0 when empty), so most misses never touch the
/// key array. The slots are split into PARTS ranges and a probe wraps within
/// its range, so each range can be filled by one thread with no locks.
struct Terms {
    slots: Vec<u64>,
    mask: usize,  // slots - 1
    range: usize, // slots per range - 1
    keys: Vec<Key>,
    lens: Vec<u8>,
    hashes: Vec<u64>,
}

const PARTS: usize = 64;

impl Terms {
    /// Room for n keys at load 1/2 or less.
    fn new(n: usize) -> Terms {
        let cap = (2 * n).next_power_of_two().max(PARTS * 64);
        let (keys, lens, hashes) = (vec![[0; 4]; n], vec![0; n], vec![0; n]);
        Terms { slots: vec![0; cap], mask: cap - 1, range: cap / PARTS - 1, keys, lens, hashes }
    }

    /// The thread of nt that fills the slots hash h starts at.
    fn owner(&self, h: u64, nt: usize) -> usize {
        (h as usize & self.mask) / (self.range + 1) % nt
    }

    fn next(&self, i: usize) -> usize {
        i & !self.range | (i + 1) & self.range
    }

    /// The id of key k (hash h, len bytes), added as new() if absent. Only the
    /// owner of h may call this.
    unsafe fn place(&self, k: &Key, h: u64, len: usize, new: impl FnOnce() -> u32) -> u32 {
        let slots = self.slots.as_ptr() as *mut u64;
        let mut i = h as usize & self.mask;
        loop {
            let s = *slots.add(i);
            if s == 0 {
                let id = new();
                self.set(id, k, h, len);
                *slots.add(i) = h & !0xFFFF_FFFF | (id as u64 + 1);
                return id;
            }
            if (s ^ h) >> 32 == 0 && self.keys[(s as u32 - 1) as usize] == *k {
                return s as u32 - 1;
            }
            i = self.next(i);
        }
    }

    /// Stores key k of id (hash h, len bytes). Only the owner of h may call this.
    unsafe fn set(&self, id: u32, k: &Key, h: u64, len: usize) {
        *(self.keys.as_ptr() as *mut Key).add(id as usize) = *k;
        *(self.lens.as_ptr() as *mut u8).add(id as usize) = len as u8;
        *(self.hashes.as_ptr() as *mut u64).add(id as usize) = h;
    }

    /// The first n terms, old id i renumbered to new_id[i] (under space). Only
    /// ids under end can be found; the others keep just their keys.
    fn renumbered(&self, n: usize, space: usize, end: u32, new_id: &[u32], nt: usize) -> Terms {
        let t = Terms::new(space);
        on_threads(nt, |th| {
            for (old, &h) in self.hashes[..n].iter().enumerate() {
                let (k, len, id) = (&self.keys[old], self.lens[old] as usize, new_id[old]);
                match (t.owner(h, nt) == th, id < end) {
                    (true, true) => _ = unsafe { t.place(k, h, len, || id) },
                    (true, false) => unsafe { t.set(id, k, h, len) },
                    _ => {}
                }
            }
        });
        t
    }
    fn prefetch(&self, h: u64) {
        unsafe { _mm_prefetch::<_MM_HINT_T0>(self.slots.as_ptr().add(h as usize & self.mask) as *const i8) }
    }

    /// k's id, or NONE; h is hash_key(k), computed (and prefetched) earlier.
    #[inline(always)]
    fn find(&self, k: &Key, h: u64) -> u32 {
        let mut i = h as usize & self.mask;
        loop {
            let s = unsafe { *self.slots.get_unchecked(i) };
            if s == 0 {
                return NONE;
            }
            if (s ^ h) >> 32 == 0 && unsafe { self.keys.get_unchecked((s as u32 - 1) as usize) } == k {
                return s as u32 - 1;
            }
            i = self.next(i);
        }
    }

    /// Is the token text tok the term id? Compared byte by byte.
    fn is(&self, tok: &[u8], id: u32) -> bool {
        let key = unsafe { &*(self.keys.as_ptr().add(id as usize) as *const [u8; 32]) };
        tok.len() == self.lens[id as usize] as usize && tok.iter().zip(key).all(|(c, &k)| c.to_ascii_lowercase() == k)
    }
}

/// Bit i is set when byte i of the 64 at p is a token byte, [A-Za-z0-9]: after
/// OR-ing 0x20 a letter lies in 'a'..'z', and a digit lies in '0'..'9' as is.
#[inline(always)]
unsafe fn token_bytes(p: *const u8) -> u64 {
    let half = |q: *const u8| {
        let v = _mm256_loadu_si256(q as *const __m256i);
        let letter = _mm256_sub_epi8(_mm256_or_si256(v, _mm256_set1_epi8(0x20)), _mm256_set1_epi8(b'a' as i8));
        let digit = _mm256_sub_epi8(v, _mm256_set1_epi8(b'0' as i8));
        let zero = _mm256_setzero_si256();
        let is_letter = _mm256_cmpeq_epi8(_mm256_subs_epu8(letter, _mm256_set1_epi8(25)), zero);
        let is_digit = _mm256_cmpeq_epi8(_mm256_subs_epu8(digit, _mm256_set1_epi8(9)), zero);
        _mm256_movemask_epi8(_mm256_or_si256(is_letter, is_digit)) as u32 as u64
    };
    half(p) | half(p.add(32)) << 32
}

/// Calls emit(term id, start) for every token of text [q, e) (NONE for tokens
/// that are not terms; tokens over 32 bytes are skipped). Token bounds come
/// from one class mask per 64-byte window; a token running into the window's
/// end starts the next window. Lookups lag their prefetch by LAG tokens.
#[inline(always)]
fn for_each_token(mut q: *const u8, e: *const u8, terms: &Terms, mut emit: impl FnMut(u32, *const u8)) {
    let mut keys = [[0u64; 4]; LAG];
    let mut hashes = [0u64; LAG];
    let mut starts = [std::ptr::null::<u8>(); LAG];
    let mut n = 0;
    let mut m = 0u64; // token bytes of the window at q not yet consumed
    let mut fresh = true;
    unsafe {
        while q < e {
            if fresh {
                m = token_bytes(q);
                let left = e.offset_from(q) as usize;
                if left < 64 {
                    m &= (1u64 << left) - 1;
                }
                fresh = false;
            }
            if m == 0 {
                q = q.add(64);
                fresh = true;
                continue;
            }
            let lead = m.trailing_zeros() as usize;
            let s = q.add(lead);
            let after = !m & (!0u64 << lead); // bytes past the token start that end it
            if after == 0 && lead != 0 {
                q = s; // re-window at the token
                fresh = true;
                continue;
            }
            let mut t = if after != 0 { q.add(after.trailing_zeros() as usize) } else { s.add(64) };
            while t < e && (*t).is_ascii_alphanumeric() {
                t = t.add(1); // only a token of 64 bytes or more
            }
            m &= if after != 0 { !0u64 << after.trailing_zeros() } else { 0 };
            if t > q.add(64) {
                q = t;
                fresh = true;
            }
            let len = t.offset_from(s) as usize;
            if len > MAX_TERM {
                continue;
            }
            let k = load_key(s, len);
            let h = hash_key(&k);
            terms.prefetch(h);
            let slot = n % LAG;
            n += 1;
            if n > LAG {
                emit(terms.find(&keys[slot], hashes[slot]), starts[slot]);
            }
            keys[slot] = k;
            hashes[slot] = h;
            starts[slot] = s;
        }
    }
    for i in n.saturating_sub(LAG)..n {
        emit(terms.find(&keys[i % LAG], hashes[i % LAG]), starts[i % LAG]);
    }
}

/// All queries, flat: query i's terms are terms[first[i]..first[i + 1]], and
/// for BOOL, ops[first[i] + k] joins its terms k and k + 1.
struct Queries {
    terms: Vec<u32>,
    first: Vec<u32>,
    ops: Vec<u8>,
    phrase: Vec<bool>,
}

struct Query<'a> {
    phrase: bool,
    terms: &'a [u32],
    ops: &'a [u8], // 'A', 'O' or 'N'
}

impl Queries {
    fn new() -> Queries {
        Queries { terms: Vec::new(), first: vec![0], ops: Vec::new(), phrase: Vec::new() }
    }
    fn len(&self) -> usize {
        self.phrase.len()
    }
    fn get(&self, i: usize) -> Query<'_> {
        let (b, e) = (self.first[i] as usize, self.first[i + 1] as usize);
        Query { phrase: self.phrase[i], terms: &self.terms[b..e], ops: &self.ops[b..e - 1] }
    }
}

/// Parses the queries on nt threads, a line-aligned range each, then gives
/// each term an id in one table. Returns the queries, the table and its size.
fn parse_queries(text: &[u8], nt: usize) -> (Queries, Terms, usize) {
    let cut = cuts(text, nt);
    // Per thread: its queries, and per term its hash, offset and length.
    let parts = on_threads(nt, |t| {
        let (mut qs, mut words) = (Queries::new(), Vec::new());
        let mut p = cut[t];
        while p < cut[t + 1] {
            let nl = p + text[p..].iter().position(|&c| c == b'\n').unwrap_or(text.len() - p);
            let phrase = text[p] == b'P';
            let (mut s, e) = if phrase { (p + 8, nl - 1) } else { (p + 5, nl) }; // past `PHRASE "` or `BOOL `
            for word in 0.. {
                if s >= e {
                    break;
                }
                let sp = text[s..e].iter().position(|&c| c == b' ').map_or(e, |k| s + k);
                if !phrase && word % 2 == 1 {
                    *qs.ops.last_mut().unwrap() = text[s];
                } else {
                    words.push((hash_key(&load_key(text[s..].as_ptr(), sp - s)), s, sp - s));
                    qs.terms.push(0);
                    qs.ops.push(0);
                }
                s = sp + 1;
            }
            qs.phrase.push(phrase);
            qs.first.push(qs.terms.len() as u32);
            p = nl + 1;
        }
        (qs, words)
    });
    let (mut all, mut words) = (Queries::new(), Vec::new());
    for (qs, w) in parts {
        let base = all.terms.len() as u32;
        all.terms.extend(qs.terms);
        all.ops.extend(qs.ops);
        all.phrase.extend(qs.phrase);
        all.first.extend(qs.first[1..].iter().map(|&f| f + base));
        words.extend(w);
    }
    // Each thread fills its own slot ranges, in slot order, so its probes walk
    // the table almost sequentially.
    let terms = Terms::new(words.len());
    let next = AtomicU32::new(0);
    let ids = Raw::of(&mut all.terms);
    on_threads(nt, |th| {
        let mut mine: Vec<usize> = (0..words.len()).filter(|&k| terms.owner(words[k].0, nt) == th).collect();
        mine.sort_unstable_by_key(|&k| words[k].0 as usize & terms.mask);
        for k in mine {
            let (h, at, len) = words[k];
            let id = unsafe { terms.place(&load_key(text[at..].as_ptr(), len), h, len, || next.fetch_add(1, Relaxed)) };
            unsafe { *ids.at(k) = id };
        }
    });
    let n = next.load(Relaxed) as usize;
    (all, terms, n)
}

/// An append-only list of 16-bit units in blocks mapped straight from the
/// kernel; drain() unmaps each block once read, so the memory really goes back.
struct Blocks {
    blocks: Vec<*mut u16>,
    fill: usize,
}
unsafe impl Send for Blocks {}

impl Blocks {
    fn new() -> Blocks {
        Blocks { blocks: Vec::new(), fill: BLOCK }
    }

    #[inline(always)]
    fn push(&mut self, x: u16) {
        if self.fill == BLOCK {
            let p = unsafe { mmap(std::ptr::null_mut(), 2 * BLOCK, 3, 0x22, -1, 0) };
            assert!(p as isize != -1, "mmap");
            self.blocks.push(p as *mut u16);
            self.fill = 0;
        }
        unsafe { *self.blocks.last().unwrap_unchecked().add(self.fill) = x };
        self.fill += 1;
    }

    fn drain(&mut self, mut f: impl FnMut(u16)) {
        let n = self.blocks.len();
        for (b, &p) in self.blocks.iter().enumerate() {
            let units = unsafe { std::slice::from_raw_parts(p, if b + 1 == n { self.fill } else { BLOCK }) };
            units.iter().for_each(|&x| f(x));
            unsafe { munmap(p as *mut c_void, 2 * BLOCK) };
        }
        self.blocks.clear();
    }
}

/// Doc-level postings for every query term, and the anchored phrases' answers.
struct Index {
    words: usize,    // u64 words per bitmap
    dense: Vec<u32>, // per term: its bitmap number, or NONE
    bits: Vec<u64>,  // the bitmaps
    // A list term's documents are stored per thread, as 16-bit offsets from the
    // thread's first document: post[seg[t][id]..seg[t][id + 1]] for thread t.
    first: Vec<u32>,    // per thread: its first document
    seg: Vec<Vec<u32>>, // per thread: nterms + 1 offsets into post
    post: Vec<u16>,
    matches: Vec<(u32, u32)>, // (query, document) per phrase found, sorted
}

/// What one thread's scan leaves behind.
struct Scan {
    count: Vec<u32>,          // per term id + 1: list postings, later the write cursor
    lists: Vec<Blocks>,      // per bucket of terms
    matches: Vec<(u32, u32)>, // (query, document) per phrase found
}

/// Calls term(id, doc) once per distinct term under `posted` of each document
/// in docs[begin..stop] (ids from first), and before that, for anchors (term
/// ids from `anchors` on), hit(anchor, offset, doc) at every occurrence.
/// Returns the number of documents.
#[allow(clippy::too_many_arguments)]
fn walk(docs: &[u8], begin: usize, stop: usize, first: u32, terms: &Terms, anchors: u32, posted: u32,
        mut term: impl FnMut(u32, u32), mut hit: impl FnMut(u32, usize, u32)) -> u32 {
    let base = docs.as_ptr();
    // Per term: 1 + the offset of the last document it was seen in.
    let mut last = vec![0u16; terms.lens.len()];
    let mut doc = first;
    let mut p = begin;
    while p < stop {
        let tab = find_byte(docs, p, b'\t');
        let nl = find_byte(docs, tab, b'\n');
        let mark = (doc - first + 1) as u16;
        for_each_token(unsafe { base.add(tab + 1) }, unsafe { base.add(nl) }, terms, |id, s| {
            if id == NONE {
                return;
            }
            if id >= anchors {
                hit(id - anchors, unsafe { s.offset_from(base) } as usize, doc);
            }
            if id < posted && last[id as usize] != mark {
                last[id as usize] = mark;
                term(id, doc);
            }
        });
        p = nl + 1;
        doc += 1;
    }
    doc - first
}

fn build_index(docs: &[u8], old: Terms, nterms: usize, qs: &mut Queries, nt: usize) -> (Index, Terms) {
    let mut ndocs = 0;
    if !docs.is_empty() {
        let last = docs[..docs.len() - 1].iter().rposition(|&c| c == b'\n').map_or(0, |i| i + 1);
        ndocs = parse_id(&docs[last..]) as usize + 1;
    }
    let words = ndocs.div_ceil(64);
    let cut = cuts(docs, nt);
    let first: Vec<u32> = (0..nt).map(|t| if cut[t] < cut[t + 1] { parse_id(&docs[cut[t]..]) } else { 0 }).collect();

    // Sample the start of each range.
    let samples = on_threads(nt, |t| {
        let mut count = vec![0u32; nterms];
        let stop = cut[t] + (cut[t + 1] - cut[t]) / 32;
        let n = walk(docs, cut[t], stop, first[t], &old, NONE, NONE, |id, _| count[id as usize] += 1, |_, _, _| {});
        (count, n)
    });
    let sampled: u64 = samples.iter().map(|s| s.1 as u64).sum();
    let mut df = vec![0u64; nterms];
    for (count, _) in &samples {
        df.iter_mut().zip(count).for_each(|(d, &c)| *d += c as u64);
    }
    drop(samples);

    // A phrase's anchor is its rarest sampled term. Renumber: anchors last; the
    // rest by sampled frequency, dealt round-robin over the buckets of the scan
    // output so that every bucket holds a similar share of the postings (the
    // scatter then never holds one big bucket twice over) while the per-term
    // state of common terms stays in a few cache lines at each bucket's start.
    let anchor_of: Vec<Option<usize>> = (0..qs.len())
        .map(|i| {
            let q = qs.get(i);
            let j = (0..q.terms.len()).min_by_key(|&j| df[q.terms[j] as usize])?;
            q.phrase.then_some(j)
        })
        .collect();
    // Postings are needed only for BOOL terms; phrases are found in the scan.
    // Classes, numbered in this order: 0 a BOOL term; 1 an anchor that is also
    // a BOOL term; 2 an anchor only; 3 neither, compared by its bytes alone.
    let (mut anchor, mut boolean) = (vec![false; nterms], vec![false; nterms]);
    for (i, j) in anchor_of.iter().enumerate() {
        let q = qs.get(i);
        match j {
            Some(j) => anchor[q.terms[*j] as usize] = true,
            None => q.terms.iter().for_each(|&id| boolean[id as usize] = true),
        }
    }
    let class: Vec<u64> = (0..nterms).map(|id| [3, 0, 2, 1][anchor[id] as usize * 2 + boolean[id] as usize]).collect();
    let lists = class.iter().filter(|&&c| c == 0).count();
    let nb = lists.div_ceil(1 << BUCKET_BITS).max(1);
    let anchors = (nb << BUCKET_BITS) as u32; // the first anchor's id
    let space = anchors as usize + nterms - lists; // ids, with gaps at bucket ends
    let posted = anchors + class.iter().filter(|&&c| c == 1).count() as u32; // ids with postings
    let end = posted + class.iter().filter(|&&c| c == 2).count() as u32; // ids in the table
    let mut order: Vec<u64> = (0..nterms)
        .map(|id| class[id] << 62 | (u32::MAX as u64 - df[id].min(u32::MAX as u64)) << 24 | id as u64)
        .collect();
    order.sort_unstable();
    let mut new_id = vec![0u32; nterms];
    let mut dense = vec![NONE; space];
    let mut ndense = 0;
    for (rank, &key) in order.iter().enumerate() {
        let id = (key & 0xFF_FFFF) as usize;
        new_id[id] = if rank < lists { (rank % nb) << BUCKET_BITS | rank / nb } else { anchors as usize + rank - lists } as u32;
        if rank < lists && df[id] * 32 > sampled {
            dense[new_id[id] as usize] = ndense;
            ndense += 1;
        }
    }
    let terms = old.renumbered(nterms, space, end, &new_id, nt);
    drop(old);
    qs.terms.iter_mut().for_each(|id| *id = new_id[*id as usize]);
    // Per anchor, the phrases it anchors: (query, position of the anchor).
    let mut phrases: Vec<(u32, u32, u32)> = anchor_of.iter().enumerate()
        .filter_map(|(i, j)| j.map(|j| (qs.get(i).terms[j] - anchors, i as u32, j as u32)))
        .collect();
    phrases.sort_unstable();
    let mut bits = vec![0u64; ndense as usize * words];
    let bits_w = Raw::of(&mut bits);

    // Scan everything. A phrase is checked where its anchor stands, while the
    // text is in cache.
    let nbuckets = (posted as usize >> BUCKET_BITS) + 1;
    let qsr = &*qs;
    let mut scans = on_threads(nt, |t| {
        let mut s = Scan { count: vec![0; space + 1], lists: (0..nbuckets).map(|_| Blocks::new()).collect(), matches: Vec::new() };
        let mut prev = vec![0u32; nbuckets]; // per bucket: offset of its last document
        // The bitmap words of this range's first and last documents.
        let edge = (first[t] / 64, (first.get(t + 1).map_or(ndocs as u32, |&f| f).max(1) - 1) / 64);
        let (count, lists, matches) = (&mut s.count, &mut s.lists, &mut s.matches);
        let n = walk(docs, cut[t], cut[t + 1], first[t], &terms, anchors, posted,
            |id, doc| {
                let d = dense[id as usize];
                if d != NONE {
                    let w = unsafe { bits_w.at(d as usize * words + doc as usize / 64) };
                    if doc / 64 == edge.0 || doc / 64 == edge.1 {
                        // A word a neighbouring thread can also write.
                        unsafe { AtomicU64::from_ptr(w) }.fetch_or(1 << (doc % 64), Relaxed);
                    } else {
                        unsafe { *w |= 1 << (doc % 64) };
                    }
                } else {
                    let (b, off) = ((id >> BUCKET_BITS) as usize, doc - first[t]);
                    let gap = off - prev[b];
                    prev[b] = off;
                    lists[b].push(((id & ((1 << BUCKET_BITS) - 1)) << GAP_BITS | gap.min(GAP)) as u16);
                    if gap >= GAP {
                        lists[b].push(gap as u16);
                    }
                    count[id as usize + 1] += 1;
                }
            },
            |a, at, doc| {
                let from = phrases.partition_point(|p| p.0 < a);
                for &(_, i, j) in phrases[from..].iter().take_while(|p| p.0 == a) {
                    let q = qsr.get(i as usize);
                    let len = terms.lens[q.terms[j as usize] as usize] as usize;
                    if matches.last() != Some(&(i, doc)) && around(docs, &terms, at, at + len, q.terms, j as usize) {
                        matches.push((i, doc));
                    }
                }
            });
        assert!(n >> DOC_BITS == 0, "too many documents per thread");
        s
    });

    // Each thread's lists take one region of post, so it writes only pages
    // its own scan output is giving back. Counts become write cursors.
    let mut base = 0;
    for s in scans.iter_mut() {
        for c in s.count.iter_mut() {
            base += std::mem::replace(c, base);
        }
    }

    // Each thread drains its own buckets into its own segment of every list.
    let mut post = vec![0u16; base as usize];
    let post_w = Raw::of(&mut post);
    std::thread::scope(|sc| {
        for s in scans.iter_mut() {
            sc.spawn(move || for b in 0..nbuckets {
            let (mut doc, mut id, mut wide) = (0u32, 0u32, false); // wide: the next unit is a whole gap
            let count = &mut s.count;
            s.lists[b].drain(|x| {
                let x = x as u32;
                if wide {
                    doc += x;
                    wide = false;
                } else {
                    id = (b as u32) << BUCKET_BITS | x >> GAP_BITS;
                    if x & GAP == GAP {
                        wide = true;
                        return;
                    }
                    doc += x & GAP;
                }
                let c = &mut count[id as usize + 1];
                unsafe { *post_w.at(*c as usize) = doc as u16 };
                *c += 1;
            });
            });
        }
    });

    // Matches come per thread in document order; a stable sort by query keeps
    // each query's documents ascending.
    let mut matches: Vec<(u32, u32)> = scans.iter_mut().flat_map(|s| std::mem::take(&mut s.matches)).collect();
    matches.sort_by_key(|m| m.0);
    matches.dedup();
    let seg: Vec<Vec<u32>> = scans.into_iter().map(|s| s.count).collect();
    let ix = Index {
        words, dense, bits, first, seg, post,
        matches,
    };
    (ix, terms)
}

/// One term's documents: a bitmap, or a sorted list.
#[derive(Clone, Copy)]
enum Post<'a> {
    Bits(&'a [u64]),
    List(&'a [u32]),
}

fn has(bits: &[u64], x: u32) -> bool {
    bits[x as usize / 64] >> (x % 64) & 1 != 0
}

/// First index i >= lo with a[i] >= x, searching outward from lo.
fn gallop(a: &[u32], mut lo: usize, x: u32) -> usize {
    let (mut hi, mut step) = (lo, 1);
    while hi < a.len() && a[hi] < x {
        lo = hi + 1;
        hi += step;
        step <<= 1;
    }
    let hi = hi.min(a.len());
    lo + a[lo..hi].partition_point(|&y| y < x)
}

/// a AND b: a bit test per element against a bitmap, else a gallop from the
/// smaller list through the larger.
fn and(a: &[u32], b: Post, out: &mut Vec<u32>) {
    out.clear();
    match b {
        Post::Bits(bits) => out.extend(a.iter().filter(|&&x| has(bits, x))),
        Post::List(b) => {
            let (small, large) = if a.len() < b.len() { (a, b) } else { (b, a) };
            let mut j = 0;
            for &x in small {
                j = gallop(large, j, x);
                if j < large.len() && large[j] == x {
                    out.push(x);
                }
            }
        }
    }
}

/// a NOT b.
fn not(a: &[u32], b: Post, out: &mut Vec<u32>) {
    out.clear();
    let mut j = 0;
    out.extend(a.iter().filter(|&&x| match b {
        Post::Bits(bits) => !has(bits, x),
        Post::List(b) => {
            j = gallop(b, j, x);
            j == b.len() || b[j] != x
        }
    }));
}

fn or(a: &[u32], b: &[u32], out: &mut Vec<u32>) {
    out.clear();
    let (mut i, mut j) = (0, 0);
    while i < a.len() && j < b.len() {
        let (x, y) = (a[i], b[j]);
        out.push(x.min(y));
        i += (x <= y) as usize;
        j += (y <= x) as usize;
    }
    out.extend_from_slice(&a[i..]);
    out.extend_from_slice(&b[j..]);
}

/// Sum of the positions of the set bits of m: bit k of a position contributes
/// 2^k for every set bit whose position has bit k set.
fn position_sum(m: u64) -> u64 {
    const SEL: [u64; 6] = [
        0xAAAA_AAAA_AAAA_AAAA, 0xCCCC_CCCC_CCCC_CCCC, 0xF0F0_F0F0_F0F0_F0F0,
        0xFF00_FF00_FF00_FF00, 0xFFFF_0000_FFFF_0000, 0xFFFF_FFFF_0000_0000,
    ];
    SEL.iter().enumerate().map(|(k, &s)| ((m & s).count_ones() as u64) << k).sum()
}

/// A bitmap term's bitmap, or None with a list term's documents decoded into list.
fn decode<'a>(ix: &'a Index, id: u32, list: &mut Vec<u32>) -> Option<&'a [u64]> {
    list.clear();
    if ix.dense[id as usize] != NONE {
        return Some(&ix.bits[ix.dense[id as usize] as usize * ix.words..][..ix.words]);
    } else {
        for (s, &first) in ix.seg.iter().zip(&ix.first) {
            let segment = &ix.post[s[id as usize] as usize..s[id as usize + 1] as usize];
            list.extend(segment.iter().map(|&x| first + x as u32));
        }
    }
    None
}

/// With phrase[j] at docs[s..t], do the j tokens before it and the tokens after
/// it spell the rest of the phrase? The document's TAB and LF bound the search.
fn around(docs: &[u8], terms: &Terms, mut s: usize, mut t: usize, phrase: &[u32], j: usize) -> bool {
    for i in (0..j).rev() {
        while !docs[s - 1].is_ascii_alphanumeric() {
            if docs[s - 1] == b'\t' {
                return false;
            }
            s -= 1;
        }
        let end = s;
        while docs[s - 1].is_ascii_alphanumeric() {
            s -= 1;
        }
        if !terms.is(&docs[s..end], phrase[i]) {
            return false;
        }
    }
    for &id in &phrase[j + 1..] {
        while !docs[t].is_ascii_alphanumeric() {
            if docs[t] == b'\n' {
                return false;
            }
            t += 1;
        }
        let start = t;
        while docs[t].is_ascii_alphanumeric() {
            t += 1;
        }
        if !terms.is(&docs[start..t], id) {
            return false;
        }
    }
    true
}

struct Solver<'a> {
    ix: &'a Index,
    qs: &'a Queries,
    cur: Vec<u32>, // the running result as a sorted list
    tmp: Vec<u32>,
    list: Vec<u32>,
    bm: Vec<u64>, // the running result as a bitmap, when dense
    dense: bool,
}

impl Solver<'_> {
    fn answer(&mut self, i: usize, out: &mut Vec<u8>) {
        let q = &self.qs.get(i);
        if q.phrase {
            self.phrase(i);
        } else {
            self.boolean(q);
        }
        self.format(out);
    }

    /// Left to right. The result is a list until a bitmap term starts the query
    /// or is OR-ed in; from then on it is a bitmap until an AND with a list.
    fn boolean(&mut self, q: &Query) {
        let Self { ix, cur, tmp, list, bm, dense, .. } = self;
        let ix = *ix;
        *dense = match decode(ix, q.terms[0], list) {
            Some(bits) => {
                bm.clear();
                bm.extend_from_slice(bits);
                true
            }
            None => {
                std::mem::swap(cur, list);
                false
            }
        };
        for (&op, &id) in q.ops.iter().zip(&q.terms[1..]) {
            let b = match decode(ix, id, list) {
                Some(bits) => Post::Bits(bits),
                None => Post::List(&list[..]),
            };
            match (*dense, b, op) {
                (true, Post::Bits(bits), _) => {
                    let pairs = bm.iter_mut().zip(bits);
                    match op {
                        b'A' => pairs.for_each(|(x, y)| *x &= y),
                        b'O' => pairs.for_each(|(x, y)| *x |= y),
                        _ => pairs.for_each(|(x, y)| *x &= !y),
                    }
                }
                (true, Post::List(l), b'A') => {
                    let bits: &[u64] = bm;
                    cur.clear();
                    cur.extend(l.iter().filter(|&&x| has(bits, x)));
                    *dense = false;
                }
                (true, Post::List(l), _) => {
                    for &x in l {
                        let (w, bit) = (x as usize / 64, 1u64 << (x % 64));
                        bm[w] = if op == b'O' { bm[w] | bit } else { bm[w] & !bit };
                    }
                }
                (false, Post::Bits(bits), b'O') => {
                    bm.clear();
                    bm.extend_from_slice(bits);
                    cur.iter().for_each(|&x| bm[x as usize / 64] |= 1 << (x % 64));
                    *dense = true;
                }
                (false, b, _) => {
                    match (op, b) {
                        (b'A', b) => and(cur, b, tmp),
                        (b'O', Post::List(l)) => or(cur, l, tmp),
                        (_, b) => not(cur, b, tmp),
                    }
                    std::mem::swap(cur, tmp);
                }
            }
        }
    }

    fn phrase(&mut self, i: usize) {
        self.dense = false;
        self.cur.clear();
        let m = &self.ix.matches;
        let from = m.partition_point(|x| x.0 < i as u32);
        self.cur.extend(m[from..].iter().take_while(|x| x.0 == i as u32).map(|x| x.1));
    }

    /// `count sum id_1 ... id_m`, with the ten smallest ids.
    fn format(&self, out: &mut Vec<u8>) {
        let (mut count, mut sum) = (0u64, 0u64);
        let mut smallest = [0u32; 10];
        let mut m = 0;
        if self.dense {
            for (w, &word) in self.bm.iter().enumerate() {
                let c = word.count_ones() as u64;
                count += c;
                sum += c * 64 * w as u64 + position_sum(word);
                let mut bits = word;
                while bits != 0 && m < 10 {
                    smallest[m] = (w * 64) as u32 + bits.trailing_zeros();
                    m += 1;
                    bits &= bits - 1;
                }
            }
        } else {
            count = self.cur.len() as u64;
            sum = self.cur.iter().map(|&x| x as u64).sum();
            m = self.cur.len().min(10);
            smallest[..m].copy_from_slice(&self.cur[..m]);
        }
        _ = write!(out, "{count} {sum}");
        smallest[..m].iter().for_each(|x| _ = write!(out, " {x}"));
        out.push(b'\n');
    }
}

/// Answers the queries on nt threads, CHUNK at a time, and returns the output.
fn answer_all(ix: &Index, qs: &Queries, nt: usize) -> Vec<u8> {
    let nchunks = qs.len().div_ceil(CHUNK);
    let next = AtomicUsize::new(0);
    let parts = on_threads(nt, |_| {
        let mut solver = Solver {
            ix, qs,
            cur: Vec::new(), tmp: Vec::new(), list: Vec::new(), bm: Vec::new(), dense: false,
        };
        let mut done = Vec::new();
        loop {
            let c = next.fetch_add(1, Relaxed);
            if c >= nchunks {
                break done;
            }
            let mut out = Vec::with_capacity(CHUNK * 32);
            for i in c * CHUNK..qs.len().min((c + 1) * CHUNK) {
                solver.answer(i, &mut out);
            }
            done.push((c, out));
        }
    });
    let mut chunks = vec![Vec::new(); nchunks];
    for (c, out) in parts.into_iter().flatten() {
        chunks[c] = out;
    }
    chunks.concat()
}

fn main() {
    unsafe {
        // With transparent huge pages a block written only in part can be
        // backed by a whole 2 MB page (PR_SET_THP_DISABLE).
        prctl(41, 1, 0, 0, 0);
        // Large blocks come from mmap and go back when freed (M_MMAP_THRESHOLD).
        mallopt(-3, 1 << 16);
    }
    let nt = std::thread::available_parallelism().map_or(1, |n| n.get()).min(12);
    let qtext = map("/data/queries.txt");
    let docs = map("/data/docs.txt");
    let (mut queries, terms, nterms) = parse_queries(qtext, nt);
    let (ix, _terms) = build_index(docs, terms, nterms, &mut queries, nt);
    let out = answer_all(&ix, &queries, nt);
    std::fs::write("/out/results.txt", out).expect("/out/results.txt");
}
