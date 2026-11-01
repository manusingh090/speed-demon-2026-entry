use std::arch::x86_64::*;
use std::borrow::Cow;
use std::fmt::Write;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU16, AtomicU32, AtomicU64, AtomicUsize, Ordering::Relaxed};
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
const PARTS: usize = 64;
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

unsafe fn hash(p: *const u8, len: usize) -> u64 {
    let load = |i| p.add(i).cast::<u64>().read_unaligned() | 0x2020_2020_2020_2020;
    let b = len as u32 * 8;
    let (w0, w1) = (_bzhi_u64(load(0), b), _bzhi_u64(load(8), b.saturating_sub(64)));
    let mut h = (w0 ^ len as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15)
        ^ w1.wrapping_mul(0xC2B2_AE3D_27D4_EB4F);
    if len > 16 {
        let w2 = _bzhi_u64(load(16), b - 128);
        let w3 = if len > 24 { _bzhi_u64(load(24), b - 192) } else { 0 };
        h ^= w2.wrapping_mul(0x1656_67B1_9E37_79F9) ^ w3.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    }
    (h ^ h >> 31).wrapping_mul(0xD6E8_FEB8_6659_FD93)
}

// Slots are probed four at a time from the start of the aligned group of four
// the hash points into: a 32-byte compare finds the term or the first free
// slot, which is where it would have been inserted. Probes stay inside the
// slot's partition (one of PARTS equal ranges), so the partitions can be
// filled by different threads at once.
struct Terms {
    slot: &'static mut [u64],
    n: usize,
}

impl Terms {
    fn find(&self, h: u64) -> u32 {
        let (sub, mut i) = (self.slot.len() / PARTS - 1, h as usize & (self.slot.len() - 4));
        unsafe {
            let (tag, keep) = (_mm256_set1_epi64x((h & !ID_MASK) as i64), _mm256_set1_epi64x(!ID_MASK as i64));
            loop {
                let v = _mm256_load_si256(self.slot.as_ptr().add(i).cast());
                let hit = _mm256_cmpeq_epi64(_mm256_and_si256(v, keep), tag);
                let found = _mm256_or_si256(hit, _mm256_cmpeq_epi64(v, _mm256_setzero_si256()));
                let m = _mm256_movemask_pd(_mm256_castsi256_pd(found)) as usize;
                if m != 0 {
                    let s = *self.slot.get_unchecked(i + m.trailing_zeros() as usize);
                    return ((s & ID_MASK) as u32).wrapping_sub(1).min(self.n as u32);
                }
                i = (i & !sub) | ((i + 4) & sub);
            }
        }
    }

    fn renumber(&mut self, new: &[u32]) {
        for s in self.slot.iter_mut().filter(|s| **s != 0) {
            *s = (*s & !ID_MASK) | (new[(*s & ID_MASK) as usize - 1] + 1) as u64;
        }
    }

}

// The id of hash h in one partition's slots, the partition's new terms
// numbered 0, 1, ... in the order they are inserted.
fn insert(slot: &mut [u64], h: u64, n: &mut u32) -> u32 {
    let (mask, mut i) = (slot.len() - 1, h as usize & (slot.len() - 4));
    while slot[i] != 0 {
        if slot[i] >> ID_BITS == h >> ID_BITS {
            return (slot[i] & ID_MASK) as u32 - 1;
        }
        i = (i + 1) & mask;
    }
    (*n += 1, slot[i] = (h & !ID_MASK) | *n as u64);
    *n - 1
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

// The ids of the document at p, appended to ids after LAG throwaway ones.
const LAG: usize = 8;
unsafe fn tokens(mut p: *const u8, t: &Terms, ids: &mut Vec<u32>, pre: *const u16) -> *const u8 {
    let mut ring = [0u64; LAG];
    let (mut n, mut out, mask) = (0usize, ids.len(), t.slot.len() - 1);
    loop {
        (ids.set_len(out), ids.reserve(32 + LAG));
        let buf = ids.as_mut_ptr();
        let mut look = |&h: &u64| {
            let id = t.find(h);
            _mm_prefetch::<_MM_HINT_T0>(pre.wrapping_add(id as usize).cast());
            (*buf.add(out), out) = (id, out + 1);
        };
        _mm_prefetch::<_MM_HINT_T0>(p.wrapping_add(2048).cast());
        let (word, nl) = classes(p);
        let stop = nl.trailing_zeros();
        let mut m = word & ((1u128 << stop) - 1) as u64;
        let mut next = 64;
        while m != 0 {
            let (s, e) = (m.trailing_zeros(), m.wrapping_add(m & m.wrapping_neg()));
            let mut len = e.trailing_zeros() - s;
            if e == 0 && nl == 0 {
                if s != 0 {
                    next = s;
                    break;
                }
                while (*p.add(next as usize)).is_ascii_alphanumeric() {
                    next += 1;
                }
                len = next;
            }
            m &= e;
            let h = if len <= 32 { hash(p.add(s as usize), len as usize) } else { 0 };
            _mm_prefetch::<_MM_HINT_T0>(t.slot.as_ptr().add(h as usize & mask).cast());
            look(&ring[n % LAG]);
            (ring[n % LAG], n) = (h, n + 1);
        }
        if nl != 0 {
            (n..n + LAG).for_each(|i| look(&ring[i % LAG]));
            ids.set_len(out);
            return p.add(stop as usize + 1);
        }
        p = p.add(next as usize);
    }
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

fn line(out: &mut String, docs: &Docs) {
    match docs {
        Docs::List(v) => {
            let sum: u64 = v.iter().map(|&d| d as u64).sum();
            write!(out, "{} {sum}", v.len()).unwrap();
            v.iter().take(10).for_each(|d| write!(out, " {d}").unwrap());
        }
        Docs::Bits(b) => {
            let (count, sum) = unsafe { bit_sums(b) };
            write!(out, "{count} {sum}").unwrap();
            let mut rem = 10;
            for (w, &x) in b.iter().enumerate() {
                let mut m = x;
                while m != 0 && rem > 0 {
                    write!(out, " {}", w as u32 * 64 + m.trailing_zeros()).unwrap();
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

fn walk(s: &[u8], first: u32, t: &Terms, pre: *const u16, mut f: impl FnMut(u32, &[u32])) -> u32 {
    let (mut p, end) = (s.as_ptr(), s.as_ptr_range().end);
    let (mut doc, mut ids) = (first, Vec::with_capacity(1 << 12));
    while p < end {
        unsafe {
            while *p != b'\t' {
                p = p.add(1);
            }
            ids.clear();
            p = tokens(p, t, &mut ids, pre);
        }
        (f(doc, &ids[LAG..]), doc += 1);
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

    fn finish(&mut self) {
        if let Some(c) = self.chunks.last_mut() {
            unsafe { c.set_len((self.at - c.as_ptr() as usize) / 2) };
        }
    }
}

fn main() {
    unsafe { mallopt(-3, 1 << 16) };
    let qtext = map("/data/queries.txt");
    let n = (qtext.len() / 8).next_power_of_two().max(PARTS * 4);
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
    // Each thread parses a twelfth of the queries, filing every term's place
    // under its table partition; then each partition is filled by one thread,
    // in query order, so the ids do not depend on the threads' timing.
    let (mask, shift) = (terms.slot.len() - 4, (terms.slot.len() / PARTS).trailing_zeros());
    let mut qcut: Vec<usize> = (0..=THREADS).map(|t| qtext.len() * t / THREADS).collect();
    for c in &mut qcut[1..THREADS] {
        while *c < qtext.len() && qtext[*c - 1] != b'\n' {
            *c += 1;
        }
    }
    let parsed = on_threads(|t| {
        let (mut spans, mut hashes, mut ops, mut parts) = (vec![], vec![], vec![], vec![vec![]; PARTS]);
        for q in qtext[qcut[t]..qcut[t + 1]].split(|&b| b == b'\n').filter(|q| !q.is_empty()) {
            let (phrase, from) = (q[0] == b'P', hashes.len() as u32);
            let start = if phrase { 8 } else { 5 };
            for (k, word) in q[start..q.len() - phrase as usize].split(|&b| b == b' ').enumerate() {
                if !phrase && k % 2 == 1 {
                    *ops.last_mut().unwrap() = word[0];
                    continue;
                }
                let h = unsafe { hash(word.as_ptr(), word.len()) };
                (parts[(h as usize & mask) >> shift].push(hashes.len() as u32), hashes.push(h), ops.push(0));
            }
            spans.push((phrase, from, hashes.len() as u32));
        }
        let ids: Vec<AtomicU32> = (0..hashes.len()).map(|_| AtomicU32::new(0)).collect();
        (spans, hashes, ops, parts, ids)
    });
    let (next, sub) = (AtomicUsize::new(0), terms.slot.len() / PARTS);
    let slices: Vec<Mutex<&mut [u64]>> = terms.slot.chunks_mut(sub).map(Mutex::new).collect();
    let sizes = on_threads(|_| {
        let mut done = vec![];
        while let p @ 0..PARTS = next.fetch_add(1, Relaxed) {
            let (mut slot, mut n) = (slices[p].lock().unwrap(), 0);
            for (_, hashes, _, parts, ids) in &parsed {
                for &k in &parts[p] {
                    let id = insert(&mut slot, hashes[k as usize], &mut n);
                    ids[k as usize].store((p as u32) << ID_BITS | id, Relaxed);
                }
            }
            done.push((p, n));
        }
        done
    });
    drop(slices);
    let mut base = [0u32; PARTS + 1];
    sizes.into_iter().flatten().for_each(|(p, n)| base[p + 1] = n);
    (0..PARTS).for_each(|p| base[p + 1] += base[p]);
    for (p, part) in terms.slot.chunks_mut(sub).enumerate() {
        part.iter_mut().filter(|s| **s != 0).for_each(|s| *s += base[p] as u64);
    }
    terms.n = base[PARTS] as usize;
    let (mut spans, mut ids, mut ops) = (vec![], vec![], vec![]);
    for (s, _, o, _, i) in parsed {
        let off = ids.len() as u32;
        spans.extend(s.into_iter().map(|(phrase, a, b)| (phrase, a + off, b + off)));
        let global = |v: u32| base[(v >> ID_BITS) as usize] + (v & ID_MASK as u32);
        ids.extend(i.into_iter().map(|v| global(v.into_inner())));
        ops.extend(o);
    }
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
            let n = walk(sample, 0, &terms, last.as_ptr(), |doc, toks| {
                let mark = (doc + 1) as u16;
                for &id in toks {
                    if std::mem::replace(&mut last[id as usize], mark) != mark {
                        df[id as usize] += 1;
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
        let pack = |i: usize| (class(i) as u64) << 56 | (!df[i] as u64) << 24 | i as u64;
        let mut packed: Vec<u64> = (0..nterms).map(pack).collect();
        packed.sort_unstable();
        let at_most = |c| packed.partition_point(|&k| (k >> 56) as usize <= c);
        let [nbits, a0, lists, nkeep] = [0, 1, 2, 3].map(at_most);
        let mut new = vec![0; nterms];
        for (r, &k) in packed.iter().enumerate() {
            new[k as usize & 0xFF_FFFF] = r as u32;
        }
        terms.renumber(&new);
        ids.iter_mut().for_each(|id| *id = new[*id as usize]);
        anchored.iter_mut().for_each(|a| a.0 = new[a.0 as usize]);
        anchored.sort_unstable();
        let first_of = |i: usize| anchored.partition_point(|a| (a.0 as usize) < i);
        let astart: Vec<usize> = (a0..=nkeep).map(first_of).collect();
        (nbits, a0, lists, nkeep, anchored, astart)
    };

    let words = ndocs.div_ceil(256) * 4;
    let bits: Vec<AtomicU64> = unsafe { std::mem::transmute(vec![0u64; nbits * words]) };
    let found = Mutex::new(vec![]);
    let scans = on_threads(|t| {
        let (mut count, mut last) = (vec![0u16; nkeep + 1], vec![0u16; nkeep + 1]);
        let (mut mine, mut kept, mut anchors) = (vec![], vec![0; 1 << 12], vec![0; 1 << 12]);
        let mut buckets: Vec<Units> = (0..BUCKETS).map(|_| Units::default()).collect();
        let first = firsts[t];
        let (mut pending, mut block) = (vec![0u64; nbits + 1], first as usize / 64);
        let flush = |pending: &mut [u64], w: usize| {
            for (b, x) in pending[..nbits].iter_mut().enumerate() {
                if *x != 0 {
                    bits[b * words + w].fetch_or(std::mem::take(x), Relaxed);
                }
            }
        };
        let (share, pre) = (&text[cut[t]..cut[t + 1]], last.as_ptr());
        walk(share, first, &terms, pre, |doc, toks| {
            if doc as usize / 64 != block {
                let w = std::mem::replace(&mut block, doc as usize / 64);
                flush(&mut pending, w);
            }
            let (mark, bit) = ((doc - first + 1) as u16, 1u64 << (doc % 64));
            let (nkeep, a0, nbits, lists, off) = (nkeep, a0, nbits, lists, doc - first);
            let (last, pending) = (&mut last[..=nkeep], &mut pending[..=nbits]);
            kept.resize(kept.len().max(toks.len()), 0);
            anchors.resize(anchors.len().max(toks.len()), 0);
            let (mut n, mut na) = (0, 0);
            for (at, &id) in toks.iter().enumerate() {
                let i = (id as usize).min(nkeep);
                unsafe {
                    *anchors.get_unchecked_mut(na) = at;
                    na += (i.wrapping_sub(a0) < nkeep - a0) as usize;
                    let new = std::mem::replace(last.get_unchecked_mut(i), mark) != mark;
                    *pending.get_unchecked_mut(i.min(nbits)) |= bit;
                    *kept.get_unchecked_mut(n) = i as u32;
                    n += (new & (i.wrapping_sub(nbits) < lists - nbits)) as usize;
                }
            }
            for &at in &anchors[..na] {
                let k = (toks[at] as usize).min(nkeep) - a0;
                for &(_, q, j) in &anchored[astart[k]..astart[k + 1]] {
                    let span = spans[q as usize];
                    let phrase = &ids[span.1 as usize..span.2 as usize];
                    // One term other than the anchor is compared first, so most
                    // windows are turned down without the slice compare's call.
                    let (s, e) = (at.wrapping_sub(j as usize), if j == 0 { phrase.len() - 1 } else { 0 });
                    let near = toks.get(s.wrapping_add(e)) == phrase.get(e);
                    if near && toks.get(s..s.wrapping_add(phrase.len())) == Some(phrase) {
                        mine.push((q, doc));
                    }
                }
            }
            for &i in &kept[..n] {
                buckets[i as usize % BUCKETS].add(i / BUCKETS as u32, off);
                count[i as usize] += 1;
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
        (count, Mutex::new(buckets))
    });
    unsafe { munmap(terms.slot.as_mut_ptr().cast(), terms.slot.len() * 8) };

    let (mut seg, mut start, mut end) = (vec![0u16; lists * THREADS], vec![0u32; lists], 0);
    for i in (0..BUCKETS).flat_map(|b| (b..lists).step_by(BUCKETS)) {
        let s = &mut seg[i * THREADS..][..THREADS];
        s.iter_mut().zip(&scans).for_each(|(n, c)| *n = c.0[i]);
        (start[i], end) = (end, end + s.iter().map(|&n| n as u32).sum::<u32>());
    }
    let buckets: Vec<_> = scans.into_iter().map(|s| s.1).collect();
    let post: Vec<AtomicU16> = unsafe { std::mem::transmute(vec![0u16; end as usize]) };
    on_threads(|t| {
        let buckets = std::mem::take(&mut *buckets[t].lock().unwrap());
        let mut cursor = vec![0u32; lists.div_ceil(BUCKETS)];
        for (b, units) in buckets.into_iter().enumerate() {
            for (c, i) in cursor.iter_mut().zip((b..lists).step_by(BUCKETS)) {
                let before: u32 = seg[i * THREADS..][..t].iter().map(|&n| n as u32).sum();
                *c = start[i] + before;
            }
            let (mut doc, mut term) = (0u32, NONE);
            for x in units.chunks.into_iter().flatten() {
                if term == NONE {
                    term = (x >> 4) as u32;
                    if x as u32 & GAP == GAP {
                        continue;
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
    let file = std::fs::File::create("/out/results.txt").expect("/out/results.txt");
    let mut out = std::io::BufWriter::new(file);
    for c in chunks {
        std::io::Write::write_all(&mut out, c.into_inner().unwrap().as_bytes()).unwrap();
    }
    std::io::Write::flush(&mut out).unwrap();
    unsafe { _exit(0) };
}
