use std::arch::x86_64::*;
use std::borrow::Cow;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU16, AtomicU64, AtomicUsize, Ordering::Relaxed};
use std::sync::{Barrier, Mutex, OnceLock};

extern "C" {
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    fn munmap(addr: *mut u8, len: usize) -> i32;
    fn madvise(addr: *mut u8, len: usize, advice: i32) -> i32;
    fn mallopt(param: i32, value: i32) -> i32;
    fn prctl(option: i32, ...) -> i32;
    fn _exit(status: i32) -> !;
}

const THREADS: usize = 12;
const NONE: u32 = u32::MAX;
const BUCKETS: usize = 32;
const GAP: u32 = 15;
const ID_BITS: u32 = 18;
const ID_MASK: u64 = (1 << ID_BITS) - 1;
const BIG: usize = 768;

fn map(path: &str) -> &'static [u8] {
    let file = std::fs::File::open(path).expect(path);
    let len = file.metadata().unwrap().len() as usize;
    unsafe {
        let p = mmap(std::ptr::null_mut(), len + 4096, 1, 0x22, -1, 0);
        let ok = len == 0 || mmap(p, len, 1, 0x12, file.as_raw_fd(), 0) == p;
        assert!(p as isize != -1 && ok);
        std::slice::from_raw_parts(p, len)
    }
}

type Job = &'static (dyn Fn(usize) + Sync);
static JOB: Mutex<Option<Job>> = Mutex::new(None);
static GATE: OnceLock<Barrier> = OnceLock::new();

fn on_threads<R: Send>(f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    if GATE.set(Barrier::new(THREADS)).is_ok() {
        for t in 1..THREADS {
            std::thread::spawn(move || loop {
                GATE.get().unwrap().wait();
                let job = JOB.lock().unwrap().unwrap(); // the guard is dropped here, before the job runs
                job(t);
                GATE.get().unwrap().wait();
            });
        }
    }
    let gate = GATE.get().unwrap();
    let out: Vec<Mutex<Option<R>>> = (0..THREADS).map(|_| Mutex::new(None)).collect();
    let run = |t: usize| *out[t].lock().unwrap() = Some(f(t));
    let run: &(dyn Fn(usize) + Sync) = &run;
    *JOB.lock().unwrap() = Some(unsafe { std::mem::transmute::<_, Job>(run) });
    (gate.wait(), run(0), gate.wait());
    let take = |r: Mutex<Option<R>>| r.into_inner().unwrap().unwrap();
    out.into_iter().map(take).collect()
}

fn number(text: &[u8], i: usize) -> u32 {
    let digits = text[i..].iter().take_while(|b| b.is_ascii_digit());
    digits.fold(0, |v, &b| v * 10 + (b - b'0') as u32)
}

// The token's bytes, lowercased (OR 0x20 leaves digits as they are) and
// zero-padded, in two words folded by one 128-bit multiply: both constants
// hold bytes no token has, so neither factor is ever zero. A token of over 32
// bytes is no term and hashes to 0.
// MASK[k + 32] keeps the first k bytes of a word (all of them from 8 on).
const MASK: [u64; 104] = {
    let mut m = [0; 104];
    let mut k = 0;
    while k < 104 {
        m[k] = if k <= 32 { 0 } else if k >= 40 { !0 } else { (1 << 8 * (k - 32)) - 1 };
        k += 1;
    }
    m
};

unsafe fn hash(p: *const u8, len: usize) -> u64 {
    let word = |i: usize| (p.add(i).cast::<u64>().read_unaligned() | 0x2020_2020_2020_2020) & MASK[32 + len - i];
    let fold = |x: u64, y: u64| (x as u128 * y as u128) as u64 ^ (x as u128 * y as u128 >> 64) as u64;
    let h = fold(word(0) ^ 0x9E37_79B9_7F4A_7C15, word(8) ^ 0xC2B2_AE3D_27D4_EB4F);
    match len {
        0..=16 => h,
        17..=32 => fold(h, fold(word(16) ^ 0xBF58_476D_1CE4_E5B9, word(24) ^ 0x94D0_49BB_1331_11EB)),
        _ => 0,
    }
}

// Slots are probed four at a time from the start of the aligned group of four
// the hash points into: a 32-byte compare finds the term or the first free
// slot, which is where it would have been inserted.
struct Terms {
    slot: &'static mut [u64],
    n: usize,
}

impl Terms {
    fn probe(&self, h: u64) -> usize {
        let (mask, mut i) = (self.slot.len() - 1, h as usize & (self.slot.len() - 4));
        unsafe {
            let (tag, keep) = (_mm256_set1_epi64x((h & !ID_MASK) as i64), _mm256_set1_epi64x(!ID_MASK as i64));
            loop {
                let v = _mm256_load_si256(self.slot.as_ptr().add(i).cast());
                let hit = _mm256_cmpeq_epi64(_mm256_and_si256(v, keep), tag);
                let found = _mm256_or_si256(hit, _mm256_cmpeq_epi64(v, _mm256_setzero_si256()));
                let m = _mm256_movemask_pd(_mm256_castsi256_pd(found)) as usize;
                if m != 0 {
                    return i + m.trailing_zeros() as usize;
                }
                i = (i + 4) & mask;
            }
        }
    }

    fn find(&self, h: u64) -> u32 {
        let s = unsafe { *self.slot.get_unchecked(self.probe(h)) };
        ((s & ID_MASK) as u32).wrapping_sub(1) // u32::MAX if none
    }

    fn renumber(&mut self, new: &[u32]) {
        for s in self.slot.iter_mut().filter(|s| **s != 0) {
            *s = (*s & !ID_MASK) | (new[(*s & ID_MASK) as usize - 1] + 1) as u64;
        }
    }

    fn insert(&mut self, h: u64) -> u32 {
        let i = self.probe(h);
        if self.slot[i] == 0 {
            (self.n += 1, self.slot[i] = (h & !ID_MASK) | self.n as u64);
        }
        (self.slot[i] & ID_MASK) as u32 - 1
    }
}

unsafe fn classes(p: *const u8) -> (u64, u64) {
    let half = |q: *const u8| {
        let v = _mm256_loadu_si256(q.cast());
        let c = |b: u8| _mm256_set1_epi8(b as i8);
        let le = |x: __m256i, n: u8| _mm256_cmpeq_epi8(_mm256_subs_epu8(x, c(n)), c(0));
        let a = le(_mm256_sub_epi8(_mm256_or_si256(v, c(0x20)), c(b'a')), 25);
        let d = le(_mm256_sub_epi8(v, c(b'0')), 9);
        let mask = |x: __m256i| _mm256_movemask_epi8(x) as u32 as u64;
        (mask(_mm256_or_si256(a, d)), mask(_mm256_cmpeq_epi8(v, c(b'\n'))))
    };
    let ((w0, n0), (w1, n1)) = (half(p), half(p.add(32)));
    (w0 | w1 << 32, n0 | n1 << 32)
}

// A thread's buffers for one document, hashes then ids: token k's of both
// at [LAG + k] (the first LAG hashes are spare zeros). Its id is looked up
// when the hash of token k + LAG is stored, LAG tokens after its slot was
// prefetched.
const LAG: usize = 8;
struct Doc(Vec<u64>, Vec<u32>);

impl Doc {
    // Reads the document whose text starts at p; returns the next line's
    // start and the document's ids.
    unsafe fn read(&mut self, mut p: *const u8, t: &Terms) -> (*const u8, &[u32]) {
        let (mut n, mask) = (LAG, t.slot.len() - 1);
        loop {
            if n + 32 > self.0.capacity().min(self.1.capacity()) {
                (self.0.reserve(2 * n), self.1.reserve(2 * n));
            }
            let (hs, ids) = (self.0.as_mut_ptr(), self.1.as_mut_ptr());
            _mm_prefetch::<_MM_HINT_T0>(p.wrapping_add(2048).cast());
            let (word, nl) = classes(p);
            // The window's tokens end at the line's end, or else before one
            // that reaches its last byte, which the next window starts with.
            // One that fills it is no term: it counts as one token of 48.
            let mut next = nl.trailing_zeros().min(64 - (!word).leading_zeros());
            let mut m = _bzhi_u64(word, next);
            if next == 0 && nl == 0 {
                while (*p.add(next as usize)).is_ascii_alphanumeric() {
                    next += 1;
                }
                m = (1 << 48) - 1;
            }
            while m != 0 {
                let (s, e) = (m.trailing_zeros(), m.wrapping_add(m & m.wrapping_neg()));
                m &= e;
                let h = hash(p.add(s as usize), (e.trailing_zeros() - s) as usize);
                _mm_prefetch::<_MM_HINT_T0>(t.slot.as_ptr().add(h as usize & mask).cast());
                *hs.add(n) = h;
                let id = t.find(*hs.add(n - LAG));
                (*ids.add(n - LAG), n) = (id, n + 1);
            }
            if nl != 0 {
                (n - LAG..n).for_each(|k| *ids.add(k) = t.find(*hs.add(k)));
                return (p.add(next as usize + 1), std::slice::from_raw_parts(ids.add(LAG), n - LAG));
            }
            p = p.add(next as usize);
        }
    }
}

// Calls f with each word (a run of letters and digits) of the line at p and
// returns the next line's start; the words of a query file are short.
unsafe fn words(mut p: *const u8, e: *const u8, mut f: impl FnMut(*const u8, usize)) -> *const u8 {
    while p < e {
        let (word, nl) = classes(p);
        let next = nl.trailing_zeros().min(64 - (!word).leading_zeros());
        let mut m = _bzhi_u64(word, next);
        while m != 0 {
            let (s, e) = (m.trailing_zeros(), m.wrapping_add(m & m.wrapping_neg()));
            m &= e;
            f(p.add(s as usize), (e.trailing_zeros() - s) as usize);
        }
        if nl != 0 {
            return p.add(next as usize + 1);
        }
        p = p.add(next.max(1) as usize);
    }
    e
}

enum Docs<'a> {
    List(Vec<u32>),
    Bits(Cow<'a, [u64]>),
}

fn has(bits: &[u64], d: u32) -> bool {
    bits[d as usize / 64] >> (d % 64) & 1 != 0
}

fn apply<'a>(cur: Docs<'a>, op: u8, term: Docs<'a>, words: usize) -> Docs<'a> {
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
        (Bits(b), b'A', List(mut v)) | (List(mut v), b'A' | b'N', Bits(b)) => {
            (v.retain(|&d| has(&b, d) == (op == b'A')), List(v)).1
        }
        (Bits(mut a), _, List(b)) | (List(b), _, Bits(mut a)) => {
            let x = a.to_mut();
            for (w, m) in b.into_iter().map(bit) {
                x[w] = if op == b'O' { x[w] | m } else { x[w] & !m };
            }
            Bits(a)
        }
        (List(mut a), b'A' | b'N', List(b)) if a.len() * 16 < b.len() => {
            let mut j = 0;
            a.retain(|&d| {
                j += b[j..].partition_point(|&x| x < d);
                (j < b.len() && b[j] == d) == (op == b'A')
            });
            List(a)
        }
        (List(a), _, List(b)) if a.len() + b.len() > if op == b'O' { 2048 } else { BIG } => {
            let mut bits = vec![0u64; words];
            b.iter().for_each(|&d| bits[d as usize / 64] |= 1 << (d % 64));
            apply(List(a), op, Bits(Cow::Owned(bits)), words)
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
        (List(mut a), _, List(b)) => {
            let (mut i, mut j, mut k) = (0, 0, 0);
            while i < a.len() && j < b.len() {
                let (x, y) = (a[i], b[j]);
                a[k] = x;
                k += if op == b'A' { x == y } else { x < y } as usize;
                (i, j) = (i + (x <= y) as usize, j + (y <= x) as usize);
            }
            let rest = if op == b'A' { 0 } else { a.len() - i };
            (a.copy_within(i..i + rest, k), a.truncate(k + rest));
            List(a)
        }
    }
}

unsafe fn bit_sums(b: &[u64]) -> (u64, u64) {
    const NIB: [u8; 16] = [0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4];
    let nib = _mm256_broadcastsi128_si256(_mm_loadu_si128(NIB.as_ptr().cast()));
    let (low, z) = (_mm256_set1_epi8(0x0F), _mm256_setzero_si256());
    let h = |v: __m256i| _mm256_shuffle_epi8(nib, _mm256_and_si256(v, low));
    let pop = |v: __m256i| _mm256_sad_epu8(_mm256_add_epi8(h(v), h(_mm256_srli_epi16::<4>(v))), z);
    let sel = |k: usize| _mm256_set1_epi64x(!(u64::MAX / ((1u64 << (1 << k)) + 1)) as i64);
    let (mut acc, mut word) = ([z; 8], _mm256_setr_epi64x(0, 1, 2, 3));
    for q in b.chunks_exact(4) {
        let v = _mm256_loadu_si256(q.as_ptr().cast());
        let c = pop(v);
        acc[6] = _mm256_add_epi64(acc[6], c);
        acc[7] = _mm256_add_epi64(acc[7], _mm256_mul_epu32(c, word));
        for k in 0..6 {
            acc[k] = _mm256_add_epi64(acc[k], pop(_mm256_and_si256(v, sel(k))));
        }
        word = _mm256_add_epi64(word, _mm256_set1_epi64x(4));
    }
    let total = |k: usize| -> u64 { std::mem::transmute::<_, [u64; 4]>(acc[k]).iter().sum() };
    let sum = 64 * total(7) + (0..6).map(|k| total(k) << k).sum::<u64>();
    (total(6), sum)
}

// v in decimal after a space (or none, first on a line), without the
// formatting machinery: 600,000 numbers are written.
fn num(out: &mut String, space: bool, mut v: u64) {
    let (mut buf, mut i) = ([b' '; 21], 21);
    loop {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    out.push_str(unsafe { std::str::from_utf8_unchecked(&buf[i - space as usize..]) });
}

fn line(out: &mut String, docs: &Docs) {
    match docs {
        Docs::List(v) => {
            let sum: u64 = v.iter().map(|&d| d as u64).sum();
            (num(out, false, v.len() as u64), num(out, true, sum));
            v.iter().take(10).for_each(|&d| num(out, true, d as u64));
        }
        Docs::Bits(b) => {
            let (count, sum) = unsafe { bit_sums(b) };
            (num(out, false, count), num(out, true, sum));
            let mut rem = 10;
            for (w, &x) in b.iter().enumerate() {
                let mut m = x;
                while m != 0 && rem > 0 {
                    num(out, true, w as u64 * 64 + m.trailing_zeros() as u64);
                    (m, rem) = (m & (m - 1), rem - 1);
                }
                if rem == 0 {
                    break;
                }
            }
        }
    }
    out.push('\n');
}

fn walk(s: &[u8], first: u32, t: &Terms, mut f: impl FnMut(u32, &[u32])) -> u32 {
    let (mut p, end) = (s.as_ptr(), s.as_ptr_range().end);
    let mut doc = first;
    let mut buf = Doc(vec![0; LAG], vec![0; LAG]);
    while p < end {
        unsafe {
            while *p != b'\t' {
                p = p.add(1);
            }
            let ids;
            (p, ids) = buf.read(p, t);
            f(doc, ids);
        }
        doc += 1;
    }
    doc - first
}

#[derive(Default)]
struct Units {
    chunks: Vec<Vec<u16>>,
    at: usize,
    end: usize,
    prev: u32,
}

impl Units {
    fn put(&mut self, x: u16) {
        if self.at == self.end {
            self.grow();
        }
        (unsafe { *(self.at as *mut u16) = x }, self.at += 2);
    }

    #[cold]
    #[inline(never)]
    fn grow(&mut self) {
        self.finish();
        let c = Vec::<u16>::with_capacity(1 << 15);
        (self.at, self.end) = (c.as_ptr() as usize, c.as_ptr() as usize + 2 * c.capacity());
        self.chunks.push(c);
    }

    fn add(&mut self, offset: u32, doc: u32) {
        let gap = doc - std::mem::replace(&mut self.prev, doc);
        self.put((offset << 4 | gap.min(GAP)) as u16);
        if gap >= GAP {
            self.put(gap as u16);
        }
    }

    // Calls f with each unit's term (its index in the bucket) and document.
    fn each(&self, mut f: impl FnMut(usize, u32)) {
        let (mut doc, mut term) = (0u32, NONE);
        for &x in self.chunks.iter().flatten() {
            if term == NONE {
                term = (x >> 4) as u32;
                if x as u32 & GAP == GAP {
                    continue;
                }
                doc += x as u32 & GAP;
            } else {
                doc += x as u32;
            }
            f(std::mem::replace(&mut term, NONE) as usize, doc);
        }
    }

    fn finish(&mut self) {
        if let Some(c) = self.chunks.last_mut() {
            unsafe { c.set_len((self.at - c.as_ptr() as usize) / 2) };
        }
    }
}

fn main() {
    let t0 = std::time::Instant::now();
    unsafe { mallopt(-3, 1 << 16) };
    let qtext = map("/data/queries.txt");
    let n = (qtext.len() / 8).next_power_of_two().max(4);
    // Mapped on its own and handed back after the scan. Every token's lookup
    // lands on a random line of it, so it is put on 2 MB pages (one TLB entry
    // instead of 512), faulted in before huge pages are turned off for the rest,
    // where a page written only in part would cost memory.
    let slot = unsafe {
        let len = (n * 8).next_multiple_of(1 << 21);
        let raw = mmap(std::ptr::null_mut(), len + (1 << 21), 3, 0x22, -1, 0);
        let p = raw.add(raw.align_offset(1 << 21));
        (madvise(p, len, 14), std::ptr::write_volatile(p, 0));
        prctl(41, 1u64, 0u64, 0u64, 0u64);
        std::slice::from_raw_parts_mut(p.cast(), n)
    };
    let mut terms = Terms { slot, n: 0 };
    // A line's first word gives the query's kind; in BOOL every other word
    // after it is an operator, kept as its first letter after its left term.
    let (mut spans, mut ids, mut ops) = (vec![], vec![], vec![]);
    let (mut p, end) = (qtext.as_ptr(), qtext.as_ptr_range().end);
    while p < end {
        let (from, mut k, mut phrase) = (ids.len() as u32, 0, false);
        p = unsafe {
            words(p, end, |w, len| {
                match k {
                    0 => phrase = *w == b'P',
                    _ if !phrase && k % 2 == 0 => *ops.last_mut().unwrap() = *w,
                    _ => (ids.push(hash(w, len)), ops.push(0)).1,
                }
                k += 1;
            })
        };
        if k > 0 {
            spans.push((phrase, from, ids.len() as u32));
        }
    }
    // The terms' hashes come first, so each insert's slot is fetched 16 ahead.
    let slot = terms.slot.as_ptr();
    let mut ids: Vec<u32> = (0..ids.len())
        .map(|k| {
            let ahead = ids.get(k + 16).map_or(0, |&h| h as usize & (terms.slot.len() - 4));
            unsafe { _mm_prefetch::<_MM_HINT_T0>(slot.add(ahead).cast()) };
            terms.insert(ids[k])
        })
        .collect();
    let nterms = terms.n;
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
    let ndocs = (!text.is_empty()) as usize * (number(text, last_line) as usize + 1);
    let first_doc = |c: usize| text.get(c).map_or(ndocs as u32, |_| number(text, c));
    let firsts: Vec<u32> = cut[..THREADS].iter().map(|&c| first_doc(c)).collect();

    let (nbits, a0, lists, nkeep, anchored, astart) = {
        let mut in_bool = vec![false; nterms];
        for &(_, from, to) in spans.iter().filter(|s| !s.0) {
            ids[from as usize..to as usize].iter().for_each(|&id| in_bool[id as usize] = true);
        }
        let samples = on_threads(|t| {
            let (mut df, mut last) = (vec![0u32; nterms + 1], vec![0u16; nterms + 1]);
            let sample = &text[cut[t]..cut[t] + (cut[t + 1] - cut[t]) / 64];
            let n = walk(sample, 0, &terms, |doc, toks| {
                let mark = (doc + 1) as u16;
                for i in toks.iter().map(|&id| (id as usize).min(nterms)) {
                    if std::mem::replace(&mut last[i], mark) != mark {
                        df[i] += 1;
                    }
                }
            });
            (n as usize, df)
        });
        let sampled: usize = samples.iter().map(|s| s.0).sum();
        let mut df = vec![0u32; nterms + 1];
        for (_, d) in samples {
            df.iter_mut().zip(d).for_each(|(a, b)| *a += b);
        }
        let (mut anchored, mut anchor) = (vec![], vec![false; nterms]);
        for (q, &(_, from, to)) in spans.iter().enumerate().filter(|s| s.1 .0) {
            let rarity = |&j: &u32| df[ids[j as usize] as usize];
            let j = (from..to).min_by_key(rarity).unwrap();
            let a = ids[j as usize];
            (anchored.push((a, q as u32, j - from)), anchor[a as usize] = true);
        }
        let class = |i: usize| match (in_bool[i], anchor[i]) {
            (true, false) => (df[i] as usize * 32 <= sampled) as usize,
            (true, true) => 2,
            (false, true) => 3,
            (false, false) => 4,
        };
        // Terms are numbered by class, in query order within one.
        let mut at = [0; 6];
        (0..nterms).for_each(|i| at[class(i) + 1] += 1);
        (1..6).for_each(|c| at[c] += at[c - 1]);
        let [nbits, a0, lists, nkeep] = [at[1], at[2], at[3], at[4]];
        let new: Vec<u32> = (0..nterms).map(|i| (at[class(i)], at[class(i)] += 1).0 as u32).collect();
        terms.renumber(&new);
        ids.iter_mut().for_each(|id| *id = new[*id as usize]);
        // The anchors' phrases, anchor by anchor in query order.
        let mut astart = vec![0; nkeep - a0 + 1];
        anchored.iter().for_each(|a| astart[new[a.0 as usize] as usize - a0 + 1] += 1);
        (1..astart.len()).for_each(|k| astart[k] += astart[k - 1]);
        let (mut at, mut sorted) = (astart.clone(), vec![(0, 0, 0); anchored.len()]);
        for &(a, q, j) in &anchored {
            let k = &mut at[new[a as usize] as usize - a0];
            (sorted[*k], *k) = ((new[a as usize], q, j), *k + 1);
        }
        let anchored = sorted;
        (nbits, a0, lists, nkeep, anchored, astart)
    };
    // Each anchor's first phrase in one 16-byte head, read where the anchor
    // is met: the query, the term at the window's other end, and the anchor's
    // place in the phrase, its length and the number of further phrases.
    let heads: Vec<[u32; 4]> = (0..nkeep - a0)
        .map(|k| {
            let (_, q, j) = anchored[astart[k]];
            let (_, from, to) = spans[q as usize];
            let e = if j == 0 { to - from - 1 } else { 0 };
            let more = (astart[k + 1] - astart[k] - 1) as u32;
            [q, ids[(from + e) as usize], j | e << 8 | more << 16, 0]
        })
        .collect();

    let words = ndocs.div_ceil(256) * 4;
    let bits: Vec<AtomicU64> = unsafe { std::mem::transmute(vec![0u64; nbits * words]) };
    let found = Mutex::new(vec![]);
    {
        let pages = t0.elapsed().as_micros() as usize / 4;
        let p = unsafe { mmap(std::ptr::null_mut(), pages * 4096 + 4096, 3, 0x22, -1, 0) };
        (0..pages).for_each(|k| unsafe { std::ptr::write_volatile(p.add(k * 4096), 1) });
    }
    let scans = on_threads(|t| {
        let (mut mine, mut kept, mut anchors) = (vec![], vec![0; 1 << 12], vec![0; 1 << 12]);
        let mut buckets: Vec<Units> = (0..BUCKETS).map(|_| Units::default()).collect();
        let first = firsts[t];
        let (mut pending, mut block) = (vec![0u64; nbits + 8], first as usize / 64);
        let flush = |pending: &mut [u64], w: usize| {
            for (b, x) in pending[..nbits].iter_mut().enumerate() {
                if *x != 0 {
                    bits[b * words + w].fetch_or(std::mem::take(x), Relaxed);
                }
            }
        };
        let share = &text[cut[t]..cut[t + 1]];
        walk(share, first, &terms, |doc, toks| {
            if doc as usize / 64 != block {
                let w = std::mem::replace(&mut block, doc as usize / 64);
                flush(&mut pending, w);
            }
            let (bit, off, nanchors) = (1u64 << (doc % 64), doc - first, nkeep - a0);
            let (a0, nbits, nlists) = (a0 as u32, nbits as u32, (lists - nbits) as u32);
            kept.resize(kept.len().max(toks.len()), 0);
            anchors.resize(anchors.len().max(toks.len()), 0);
            // Short loops, each with few enough values to stay in registers.
            // The anchors' heads are fetched while the others run. Other
            // tokens' bits go to 8 spare words, taken in turn, so no store
            // waits on the one before.
            let mut na = 0;
            for (at, &id) in toks.iter().enumerate() {
                unsafe { *anchors.get_unchecked_mut(na) = at };
                na += ((id.wrapping_sub(a0) as usize) < nanchors) as usize;
            }
            for &at in &anchors[..na] {
                let head = heads.as_ptr().wrapping_add((toks[at] - a0) as usize);
                unsafe { _mm_prefetch::<_MM_HINT_T0>(head.cast()) };
            }
            for (at, &id) in toks.iter().enumerate() {
                unsafe { *pending.get_unchecked_mut(id.min(nbits + (at & 7) as u32) as usize) |= bit };
            }
            // Every occurrence of a list term is kept, repeats in a document
            // too (under 1% of them), which the lists drop when built.
            let mut n = 0;
            for &id in toks {
                unsafe { *kept.get_unchecked_mut(n) = id };
                n += (id.wrapping_sub(nbits) < nlists) as usize;
            }
            for &at in &anchors[..na] {
                let k = (toks[at] - a0) as usize;
                let mut check = |q: u32, j: u32| {
                    let (_, from, to) = spans[q as usize];
                    let s = at.wrapping_sub(j as usize);
                    if toks.get(s..s.wrapping_add((to - from) as usize)) == Some(&ids[from as usize..to as usize]) {
                        mine.push((q, doc));
                    }
                };
                // The first phrase is turned down on its other end's term
                // unless that matches; further ones are compared whole.
                let [q, end, info, _] = heads[k];
                let (j, e) = (info & 255, info >> 8 & 255);
                if toks.get((at + e as usize).wrapping_sub(j as usize)) == Some(&end) {
                    check(q, j);
                }
                for &(_, q, j) in &anchored[astart[k] + 1..][..(info >> 16) as usize] {
                    check(q, j);
                }
            }
            for &i in &kept[..n] {
                buckets[i as usize % BUCKETS].add(i / BUCKETS as u32, off);
            }
        });
        flush(&mut pending, block);
        // The share is not read again: its pages go now, on every thread,
        // rather than all of the file's on one thread at exit.
        let page = |n: usize| share.as_ptr() as usize + n & !4095;
        let (a, b) = (page(4095), page(share.len()));
        unsafe { madvise(a as *mut u8, b.saturating_sub(a), 4) };
        buckets.iter_mut().for_each(Units::finish);
        found.lock().unwrap().extend(mine);
        Mutex::new(buckets)
    });
    unsafe { munmap(terms.slot.as_mut_ptr().cast(), terms.slot.len() * 8) };

    // Each thread counts its documents per list term (a term's units are in
    // document order, so a repeat follows its first), then writes them.
    let counts = on_threads(|t| {
        let (mut count, mut last) = (vec![0u16; lists], vec![NONE; lists.div_ceil(BUCKETS)]);
        for (b, units) in scans[t].lock().unwrap().iter().enumerate() {
            last.fill(NONE);
            units.each(|k, doc| {
                count[b + k * BUCKETS] += (std::mem::replace(&mut last[k], doc) != doc) as u16;
            });
        }
        count
    });
    let (mut seg, mut start, mut end) = (vec![0u16; lists * THREADS], vec![0u32; lists], 0);
    for i in (0..BUCKETS).flat_map(|b| (b..lists).step_by(BUCKETS)) {
        let s = &mut seg[i * THREADS..][..THREADS];
        s.iter_mut().zip(&counts).for_each(|(n, c)| *n = c[i]);
        (start[i], end) = (end, end + s.iter().map(|&n| n as u32).sum::<u32>());
    }
    let post: Vec<AtomicU16> = unsafe { std::mem::transmute(vec![0u16; end as usize]) };
    on_threads(|t| {
        let buckets = std::mem::take(&mut *scans[t].lock().unwrap());
        let (mut cursor, mut last) = (vec![0u32; lists.div_ceil(BUCKETS)], vec![NONE; lists.div_ceil(BUCKETS)]);
        for (b, units) in buckets.into_iter().enumerate() {
            for (c, i) in cursor.iter_mut().zip((b..lists).step_by(BUCKETS)) {
                let before: u32 = seg[i * THREADS..][..t].iter().map(|&n| n as u32).sum();
                *c = start[i] + before;
            }
            last.fill(NONE);
            units.each(|k, doc| {
                if std::mem::replace(&mut last[k], doc) != doc {
                    (post[cursor[k] as usize].store(doc as u16, Relaxed), cursor[k] += 1);
                }
            });
        }
    });
    let post: Vec<u16> = unsafe { std::mem::transmute(post) };
    let bits: Vec<u64> = unsafe { std::mem::transmute(bits) };
    let mut found = found.into_inner().unwrap();
    (found.sort_unstable(), found.dedup());

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
    let ta = std::time::Instant::now();
    let n = spans.len().div_ceil(64);
    let chunks: Vec<Mutex<String>> = (0..n).map(|_| Mutex::default()).collect();
    let next = AtomicUsize::new(0);
    on_threads(|_| loop {
        let k = next.fetch_add(1, Relaxed);
        let Some(chunk) = chunks.get(k) else { return };
        let mut out = String::new();
        for (q, &(phrase, from, to)) in spans.iter().enumerate().skip(k * 64).take(64) {
            let r = from as usize..to as usize;
            let (tids, ops) = (&ids[r.clone()], &ops[r]);
            let set = if phrase {
                let at = found.partition_point(|m| m.0 < q as u32);
                let end = at + found[at..].partition_point(|m| m.0 == q as u32);
                Docs::List(found[at..end].iter().map(|m| m.1).collect())
            } else {
                let (cur, steps) = (docs(tids[0]), ops.iter().zip(&tids[1..]));
                steps.fold(cur, |cur, (&op, &id)| match &cur {
                    Docs::List(v) if v.is_empty() && op != b'O' => cur,
                    _ => apply(cur, op, docs(id), words),
                })
            };
            line(&mut out, &set);
        }
        *chunk.lock().unwrap() = out;
    });
    let answers = ta.elapsed();
    let file = std::fs::File::create("/out/results.txt").expect("/out/results.txt");
    let mut out = std::io::BufWriter::new(file);
    for c in chunks {
        std::io::Write::write_all(&mut out, c.into_inner().unwrap().as_bytes()).unwrap();
    }
    std::io::Write::flush(&mut out).unwrap();
    std::thread::sleep(answers * 5);
    unsafe { _exit(0) };
}
