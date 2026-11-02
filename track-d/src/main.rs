//! Speed Demon 2026, Track D (Traverse), in Rust with no dependencies.
//!
//! 1. Parse once. graph.txt is memory-mapped and cut into one line-aligned
//!    range per thread. Each thread files every edge, as sequential appends,
//!    under the part of the node range its tail falls in (about 107 parts of
//!    16,384 nodes). An edge is one 32-bit word: the head in the low bits, the
//!    weight above it; a weight too large for its field is stored as all ones
//!    with its value on a side list.
//! 2. Build each part on its own. A thread takes a part, counts its nodes'
//!    out-degrees and places its edges, all inside a 64 KB counter array, so
//!    the adjacency list is built with no atomics and no cache misses beyond
//!    the writes themselves.
//! 3. Renumber for locality. Node ids are a random relabelling, so each thread
//!    cuts the road network into compact blobs of 512 nodes grown by BFS,
//!    copying each node's edges into its own region of the new edge array as
//!    it orders it; one pass then maps the heads to new ids and counts the
//!    in-degrees, and the graph is reversed.
//! 4. Bidirectional Dijkstra per query, with 4-ary heaps and distances kept in
//!    pages handed out on first touch, as u32 unless the answer may not fit.
//!    A target with no in-edge is -1 at once. The graph's pages are handed
//!    back on every thread at the end, so the exit has little left to free.
use std::arch::x86_64::{_mm_prefetch, _MM_HINT_T0};
use std::ffi::c_void;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU8, AtomicUsize, Ordering::Relaxed};
use std::sync::{Barrier, Mutex, Once, OnceLock};

extern "C" {
    fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut c_void;
    fn prctl(op: i32, a: u64, b: u64, c: u64, d: u64) -> i32;
    fn mallopt(param: i32, value: i32) -> i32;
    fn madvise(addr: *mut c_void, len: usize, advice: i32) -> i32;
}

const INF: u64 = u64::MAX;
const BLOB: usize = 512; // nodes per locality blob
const PART_BITS: u32 = 14; // nodes per part: 2^14, so offsets fit 16 bits
const PAGE: usize = 512; // distances per page

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

fn prefetch<T>(p: *const T) {
    unsafe { _mm_prefetch::<_MM_HINT_T0>(p as *const i8) }
}

fn map(path: &str) -> &'static [u8] {
    let file = std::fs::File::open(path).expect(path);
    let len = file.metadata().unwrap().len() as usize;
    // An anonymous page of zeros, then the file over its start (PROT_READ;
    // MAP_PRIVATE | MAP_ANONYMOUS, then MAP_PRIVATE | MAP_FIXED), so an
    // 8-byte read at any byte of the file stays inside the mapping.
    unsafe {
        let p = mmap(std::ptr::null_mut(), len + 4096, 1, 0x22, -1, 0);
        let ok = len == 0 || mmap(p, len, 1, 0x12, file.as_raw_fd(), 0) == p;
        assert!(p as isize != -1 && ok, "mmap {path}");
        std::slice::from_raw_parts(p as *const u8, len)
    }
}

/// Hands thread t's share of the whole pages inside b back to the kernel.
/// MADV_DONTNEED only takes the address-space lock for reading, so threads
/// free pages in parallel and alongside page faults, where a free or the exit
/// would do it on one thread. b must not be read afterwards, only dropped.
fn drop_pages<T>(b: &[T], t: usize, nt: usize) {
    let lo = (b.as_ptr() as usize).next_multiple_of(4096);
    let hi = (b.as_ptr() as usize + std::mem::size_of_val(b)) & !4095;
    let r = share(hi.saturating_sub(lo) / 4096, t, nt);
    unsafe { madvise((lo + r.start * 4096) as *mut c_void, r.len() * 4096, 4) };
}

/// The number at text[*i..], leaving *i past the one byte that ends it.
/// Digits have bit 0x10 set and the space and newline that end a number do
/// not, so one 8-byte load finds the end of any number up to 7 digits, and
/// its digits are combined in pairs, quads and halves; longer numbers (only
/// heavy weights) take the byte loop.
#[inline(always)]
fn number(text: &[u8], i: &mut usize) -> u32 {
    let w = u64::from_le_bytes(unsafe { *(text.as_ptr().add(*i) as *const [u8; 8]) });
    let stop = !w & 0x1010_1010_1010_1010;
    if stop == 0 {
        let mut v = 0u32;
        while let d @ b'0'..=b'9' = unsafe { *text.get_unchecked(*i) } {
            v = v * 10 + (d - b'0') as u32;
            *i += 1;
        }
        *i += 1;
        return v;
    }
    let len = stop.trailing_zeros() / 8; // digits before the separator
    *i += len as usize + 1;
    let mut v = (w << (64 - 8 * len)) & 0x0F0F_0F0F_0F0F_0F0F;
    v = (v * 10 + (v >> 8)) & 0x00FF_00FF_00FF_00FF;
    v = (v * 100 + (v >> 16)) & 0x0000_FFFF_0000_FFFF;
    ((v * 10000 + (v >> 32)) & 0xFFFF_FFFF) as u32
}

/// k line-aligned cuts of text[from..].
fn cuts(text: &[u8], from: usize, k: usize) -> Vec<usize> {
    let mut c = vec![text.len(); k + 1];
    c[0] = from;
    for t in 1..k {
        let mut x = (from + (text.len() - from) * t / k).max(c[t - 1]);
        while x < text.len() && text[x - 1] != b'\n' {
            x += 1;
        }
        c[t] = x;
    }
    c
}

type Job = &'static (dyn Fn(usize) + Sync);
static POOL: OnceLock<(Mutex<Option<Job>>, Barrier)> = OnceLock::new();
static STARTED: Once = Once::new();

/// Runs f(t) on nt threads and collects what each returns. The threads are
/// started once and reused by every phase: a new thread's stack is a new
/// mapping, which waits behind every page fault and unmap in flight, so
/// twelve fresh threads per phase cost milliseconds each time.
fn on_threads<R: Send>(nt: usize, f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    let (job, gate) = POOL.get_or_init(|| (Mutex::new(None), Barrier::new(nt)));
    STARTED.call_once(|| {
        for t in 1..nt {
            std::thread::spawn(move || loop {
                let (job, gate) = POOL.get().unwrap();
                gate.wait();
                let f = job.lock().unwrap().unwrap();
                f(t);
                gate.wait();
            });
        }
    });
    let out: Vec<Mutex<Option<R>>> = (0..nt).map(|_| Mutex::new(None)).collect();
    let run = |t: usize| *out[t].lock().unwrap() = Some(f(t));
    let run: &(dyn Fn(usize) + Sync) = &run;
    // Safe while `run` outlives the phase, and the second wait ensures that.
    *job.lock().unwrap() = Some(unsafe { std::mem::transmute::<_, Job>(run) });
    gate.wait();
    run(0);
    gate.wait();
    out.into_iter().map(|r| r.into_inner().unwrap().unwrap()).collect()
}

/// Running sums of v in place (v[i] += v[i - 1]), each thread a share.
fn prefix_sum(nt: usize, v: &mut [u32]) {
    let (n, r) = (v.len(), Raw::of(v));
    let sums: Vec<u32> = on_threads(nt, |t| share(n, t, nt).map(|i| unsafe { *r.at(i) }).sum());
    on_threads(nt, |t| {
        let mut acc: u32 = sums[..t].iter().sum();
        for i in share(n, t, nt) {
            unsafe { (acc, *r.at(i)) = (acc + *r.at(i), acc + *r.at(i)) };
        }
    });
}

/// Thread t's share of 0..n.
fn share(n: usize, t: usize, nt: usize) -> std::ops::Range<usize> {
    n * t / nt..n * (t + 1) / nt
}

/// A graph's edge offsets in about 1.5 bytes a node instead of 4: a u32 for
/// each group of 2^gb nodes and a byte per node to add to it. gb is 3 unless
/// some group has more than 255 edges before its last node; then it is 0 and
/// the u32s are the offsets themselves.
struct Offs {
    base: Vec<u32>,
    rel: Vec<u8>,
    gb: u32,
}

impl Offs {
    fn new(n: usize, gb: u32) -> Self {
        Offs { base: vec![0; (n >> gb) + 1], rel: vec![0; n + 1], gb }
    }
    /// The offset of node v (of n + 1 entries, the last one the end).
    fn at(&self, v: usize) -> u32 {
        unsafe { *self.base.get_unchecked(v >> self.gb) + *self.rel.get_unchecked(v) as u32 }
    }
    /// Packs plain offsets, each thread a share of the groups. Offsets only
    /// grow, so a group fits a byte if its last one is under 256 past its first.
    fn pack(nt: usize, v: &[u32]) -> Self {
        let n = v.len() - 1;
        let fits = on_threads(nt, |t| share(n / 8 + 1, t, nt).all(|g| v[(8 * g + 7).min(n)] - v[8 * g] < 256));
        let mut o = Offs::new(n, if fits.iter().all(|&f| f) { 3 } else { 0 });
        let (gb, b, r) = (o.gb, Raw::of(&mut o.base), Raw::of(&mut o.rel));
        on_threads(nt, |t| {
            for g in share((n >> gb) + 1, t, nt) {
                let (lo, hi) = (g << gb, ((g + 1) << gb).min(n + 1));
                unsafe { *b.at(g) = v[lo] };
                v[lo..hi].iter().zip(lo..).for_each(|(&x, i)| unsafe { *r.at(i) = (x - v[lo]) as u8 });
            }
        });
        o
    }
}

/// A distance as a search stores it. Searches keep u32, which halves the
/// memory they touch, and a query whose answer may not fit is run again
/// with u64.
trait Len: Copy + Ord + Default + From<u32> + Into<u64> + TryFrom<u64> {
    const MAX: Self; // marks a node not reached
}
impl Len for u32 {
    const MAX: Self = u32::MAX;
}
impl Len for u64 {
    const MAX: Self = u64::MAX;
}

const SPAN: usize = 4096; // keys the bucket queue covers ahead of its minimum
const NIL: u32 = u32::MAX;

/// A monotone priority queue for Dijkstra (Dial's buckets, circular): a key
/// less than SPAN above the current minimum goes into bucket key % SPAN, which
/// then holds that key only, as a list threaded through one entry array whose
/// freed slots are reused; a bitmap finds the next nonempty bucket. Keys
/// further ahead (only the injected heavy edges here) wait in a short list. Push
/// and pop take a few instructions where a heap's pop sifts through every
/// level.
struct Queue {
    cur: u64, // the smallest key queued, once min has run
    len: usize,
    bits: [u64; SPAN / 64],
    head: Vec<u32>,        // bucket -> its first entry
    ent: Vec<(u32, u32)>,  // (node, next entry in its bucket or free list)
    free: u32,
    far: Vec<(u64, u32)>,
}

impl Queue {
    fn new() -> Self {
        Queue { cur: 0, len: 0, bits: [0; SPAN / 64], head: vec![NIL; SPAN], ent: Vec::new(), free: NIL, far: Vec::new() }
    }
    /// Queues node v at key k, never below the current minimum.
    #[inline(always)]
    fn push(&mut self, k: u64, v: u32) {
        self.len += 1;
        if k - self.cur >= SPAN as u64 {
            return self.far.push((k, v));
        }
        let b = k as usize % SPAN;
        let e = (v, self.head[b]);
        let i = if self.free == NIL {
            self.ent.push(e);
            self.ent.len() - 1
        } else {
            let i = self.free as usize;
            (self.free, self.ent[i]) = (self.ent[i].1, e);
            i
        };
        self.head[b] = i as u32;
        self.bits[b / 64] |= 1 << (b % 64);
    }
    /// The smallest key queued, which becomes the current minimum.
    #[inline(always)]
    fn min(&mut self) -> Option<u64> {
        if self.len == 0 {
            return None;
        }
        // The first nonempty bucket at or after cur's, circularly.
        let (from, words) = (self.cur as usize % SPAN, SPAN / 64);
        let mut gap = match self.bits[from / 64] >> (from % 64) {
            0 => (1..=words).find_map(|i| match self.bits[(from / 64 + i) % words] {
                0 => None,
                w => Some(i * 64 + w.trailing_zeros() as usize - from % 64),
            }),
            w => Some(w.trailing_zeros() as usize),
        }
        .map_or(INF, |g| self.cur + g as u64);
        if let Some(&(k, _)) = self.far.iter().min() {
            gap = gap.min(k);
        }
        self.cur = gap;
        Some(gap)
    }
    /// Takes an entry at the current minimum (min must have found one); also
    /// returns the next node in its bucket, if any.
    #[inline(always)]
    fn pop(&mut self) -> (u64, u32, Option<u32>) {
        self.len -= 1;
        let b = self.cur as usize % SPAN;
        let i = self.head[b];
        if i == NIL {
            let j = (0..self.far.len()).min_by_key(|&j| self.far[j]).unwrap();
            let (k, v) = self.far.swap_remove(j);
            return (k, v, None);
        }
        let (v, next) = self.ent[i as usize];
        (self.head[b], self.ent[i as usize].1, self.free) = (next, self.free, i);
        if next == NIL {
            self.bits[b / 64] &= !(1 << (b % 64));
            return (self.cur, v, None);
        }
        (self.cur, v, Some(self.ent[next as usize].0))
    }
    fn clear(&mut self) {
        for (w, bits) in self.bits.iter_mut().enumerate() {
            while *bits != 0 {
                self.head[w * 64 + bits.trailing_zeros() as usize] = NIL;
                *bits &= *bits - 1;
            }
        }
        (self.cur, self.len, self.free) = (0, 0, NIL);
        self.ent.clear();
        self.far.clear();
    }
}

/// Tentative distances in pages of 512, handed out on first touch and taken
/// back after each query, so a thread's memory follows its largest search.
struct Dist<T> {
    tab: Vec<u16>,
    used: Vec<u32>,
    pool: Vec<T>,
}

impl<T: Len> Dist<T> {
    fn new(n: usize) -> Self {
        Dist { tab: vec![0; n / PAGE + 1], used: Vec::new(), pool: Vec::new() }
    }
    /// INF for a node this search never reached; hands out no page.
    fn get(&self, v: u32) -> u64 {
        match unsafe { *self.tab.get_unchecked(v as usize / PAGE) } as usize {
            0 => INF,
            pg => match unsafe { *self.pool.get_unchecked((pg - 1) * PAGE + v as usize % PAGE) } {
                d if d == T::MAX => INF,
                d => d.into(),
            },
        }
    }
    #[inline(always)]
    fn at(&mut self, v: u32) -> &mut T {
        let i = v as usize / PAGE;
        if unsafe { *self.tab.get_unchecked(i) } == 0 {
            self.page(i);
        }
        let k = (unsafe { *self.tab.get_unchecked(i) } as usize - 1) * PAGE + v as usize % PAGE;
        unsafe { self.pool.get_unchecked_mut(k) }
    }
    /// Hands out a page for nodes i * PAGE.. on their first touch.
    #[cold]
    fn page(&mut self, i: usize) {
        self.used.push(i as u32);
        self.tab[i] = self.used.len() as u16;
        let s = (self.used.len() - 1) * PAGE;
        self.pool.resize(self.pool.len().max(s + PAGE), T::MAX);
        self.pool[s..s + PAGE].fill(T::MAX);
    }
    fn reset(&mut self) {
        self.used.iter().for_each(|&i| self.tab[i as usize] = 0);
        self.used.clear();
    }
}

/// One direction's graph: offsets, packed edge words, heavy weights, and the
/// bits an edge word gives the head.
type Graph<'a> = (&'a Offs, &'a [u32], &'a [(u32, u32)], u32);

/// One side of a search, with distances stored as T.
struct Side<T> {
    dist: Dist<T>,
    queue: Queue,
}

impl<T: Len> Side<T> {
    fn new(n: usize) -> Self {
        Side { dist: Dist::new(n), queue: Queue::new() }
    }

    /// Settles the next queued node and relaxes its edges; returns the best
    /// length of a path through a node the other side has reached. A length
    /// that does not fit T is dropped and *over set.
    fn settle(&mut self, g: Graph, other: &Dist<T>, mut best: u64, over: &mut bool) -> u64 {
        let (off, adj, heavy, hb) = g;
        let (hmask, big) = ((1 << hb) - 1, u32::MAX >> hb);
        let (d, u, next) = self.queue.pop();
        if let Some(next) = next {
            // Its offsets were fetched when it was queued; start on its edges.
            prefetch(adj.as_ptr().wrapping_add(off.at(next as usize) as usize));
        }
        if d > self.dist.get(u) {
            return best;
        }
        for e in off.at(u as usize)..off.at(u as usize + 1) {
            let x = unsafe { *adj.get_unchecked(e as usize) };
            let (v, w) = (x & hmask, x >> hb);
            let w = if w == big { heavy_weight(heavy, e) } else { w };
            let nd = d + w as u64;
            if nd < best {
                let Some(nt) = T::try_from(nd).ok().filter(|&x| x != T::MAX) else {
                    *over = true;
                    continue;
                };
                let slot = self.dist.at(v);
                if nt < *slot {
                    *slot = nt;
                    self.queue.push(nd, v);
                    prefetch(off.rel.as_ptr().wrapping_add(v as usize));
                    best = best.min(nd.saturating_add(other.get(v)));
                }
            }
        }
        best
    }
}

/// Bidirectional Dijkstra from s to t over g (forward, reverse): grow the
/// side with the smaller queue, record the best length where the sides meet,
/// and stop once the two smallest queued keys add up to at least that. A side
/// that runs dry has reached every node it can, so the best meeting seen is
/// the answer. Also says whether a length too large for T was dropped; the
/// answer is exact all the same if it is below T::MAX.
fn search<T: Len>(side: &mut [Side<T>; 2], g: [Graph; 2], s: u32, t: u32) -> (u64, bool) {
    let (mut best, mut over) = (INF, false);
    for (x, v) in side.iter_mut().zip([s, t]) {
        *x.dist.at(v) = 0.into();
        x.queue.push(0, v);
    }
    while let (Some(a), Some(b)) = (side[0].queue.min(), side[1].queue.min()) {
        if a + b >= best {
            break;
        }
        let [f, r] = &mut *side;
        best = match r.queue.len < f.queue.len {
            true => r.settle(g[1], &f.dist, best, &mut over),
            false => f.settle(g[0], &r.dist, best, &mut over),
        };
    }
    for x in side.iter_mut() {
        x.queue.clear();
        x.dist.reset();
    }
    (best, over)
}

/// One thread's parse output. A record (the tail's offset in its part, the
/// edge word) goes into chunks of CHUNK carved in turn from one arena of each,
/// so the 100-odd lists per thread cost one mapping instead of one each (an
/// unmap stalls every thread). Pages of the arena never written cost nothing.
struct Filed {
    offs: Vec<u16>,
    words: Vec<u32>,
    chunks: Vec<Vec<u32>>, // part -> its chunks, in order
    len: Vec<usize>,       // part -> records
    heavy: Vec<Vec<u32>>,  // part -> its heavy weights, in order
}

const CHUNK: usize = 1024;

impl Filed {
    fn records(&self, p: usize) -> impl Iterator<Item = (usize, u32)> + '_ {
        self.chunks[p].iter().enumerate().flat_map(move |(j, &c)| {
            let r = c as usize * CHUNK..c as usize * CHUNK + (self.len[p] - j * CHUNK).min(CHUNK);
            self.offs[r.clone()].iter().map(|&o| o as usize).zip(self.words[r].iter().copied())
        })
    }
}

/// The weight of edge e stored as all ones: its entry in the sorted list.
fn heavy_weight(heavy: &[(u32, u32)], e: u32) -> u32 {
    heavy[heavy.partition_point(|h| h.0 < e)].1
}

fn main() {
    // Plain pages (a part-touched array with huge pages would count whole
    // 2 MB pages), and freed arrays really go back to the kernel.
    unsafe {
        prctl(41, 1, 0, 0, 0); // PR_SET_THP_DISABLE
        mallopt(-3, 1 << 16); // M_MMAP_THRESHOLD
    }
    let nt = std::thread::available_parallelism().map_or(1, |n| n.get()).clamp(1, 12);
    let qtext = map("/data/queries.txt");
    let mut queries = Vec::new();
    let mut i = 0;
    while i < qtext.len() {
        let s = number(qtext, &mut i);
        queries.push((s, number(qtext, &mut i)));
    }
    let text = map("/data/graph.txt");
    let mut i = 0;
    let n = number(text, &mut i) as usize;
    number(text, &mut i);
    let hb = 32 - ((n + 8 * nt) as u32 | 1).leading_zeros(); // bits for a node id, padding ids included
    let (hmask, big) = ((1u32 << hb) - 1, u32::MAX >> hb);
    let sh = PART_BITS.min(hb);
    let parts = (n >> sh) + 1;

    // 1. Parse once, filing each edge under its tail's part.
    let cut = cuts(text, i, nt);
    let filed: Vec<Filed> = on_threads(nt, |t| {
        // A line takes at least 6 bytes, and each part leaves one chunk part full.
        let cap = (cut[t + 1] - cut[t]) / 6 + parts * CHUNK;
        let (mut at, mut next) = (vec![0; parts], 0); // where each part's next record goes
        let (offs, words) = (vec![0; cap], vec![0; cap]);
        let mut f = Filed { offs, words, chunks: vec![vec![]; parts], len: vec![0; parts], heavy: vec![vec![]; parts] };
        let mut i = cut[t];
        while i < cut[t + 1] {
            let u = number(text, &mut i);
            let v = number(text, &mut i);
            let mut w = number(text, &mut i);
            if u == v {
                continue; // a self-loop never shortens a path
            }
            let p = (u >> sh) as usize;
            if f.len[p] % CHUNK == 0 {
                (at[p], next) = (next * CHUNK, next + 1);
                f.chunks[p].push(next as u32 - 1);
            }
            if w >= big {
                f.heavy[p].push(w);
                w = big;
            }
            (f.offs[at[p]], f.words[at[p]]) = ((u & ((1 << sh) - 1)) as u16, v | w << hb);
            (at[p], f.len[p]) = (at[p] + 1, f.len[p] + 1);
        }
        f
    });
    // Part p's edges start where part p - 1's end.
    let mut base = vec![0u32; parts + 1];
    for p in 0..parts {
        base[p + 1] = base[p] + filed.iter().map(|f| f.len[p] as u32).sum::<u32>();
    }

    // 2. Build each part's adjacency on its own.
    // Parts start on whole groups of offsets, so a thread writes its part's
    // packed offsets itself; if a group's edges overflow a byte, it is all
    // done again with plain ones (gb = 0).
    let m = base[parts] as usize;
    let mut link = vec![0u32; m];
    let lr = Raw::of(&mut link);
    let build = |gb: u32| {
        let mut first = Offs::new(n, gb);
        let (fb, fr, over) = (Raw::of(&mut first.base), Raw::of(&mut first.rel), AtomicBool::new(false));
        let claim_part = AtomicUsize::new(0);
        let heavy: Vec<(u32, u32)> = on_threads(nt, |_| {
        let (mut heavy, mut cursor) = (Vec::new(), vec![0u32; 1 << sh]);
        loop {
            let p = claim_part.fetch_add(1, Relaxed);
            if p >= parts {
                return heavy;
            }
            let lo = p << sh;
            let len = (n - lo).min(1 << sh);
            cursor.fill(0);
            filed.iter().for_each(|f| f.records(p).for_each(|(o, _)| cursor[o] += 1));
            let (mut at, mut group, mut gap) = (base[p], 0, 0);
            for (k, c) in cursor[..len].iter_mut().enumerate() {
                if (lo + k) % (1 << gb) == 0 {
                    (group, _) = (at, unsafe { *fb.at((lo + k) >> gb) = at });
                }
                (gap, _) = (gap.max(at - group), unsafe { *fr.at(lo + k) = (at - group) as u8 });
                (at, *c) = (at + *c, at);
            }
            if gap > 255 {
                over.store(true, Relaxed);
            }
            for f in &filed {
                let mut h = f.heavy[p].iter();
                f.records(p).for_each(|(o, x)| {
                    let c = &mut cursor[o];
                    if x >> hb == big {
                        heavy.push((*c, *h.next().unwrap()));
                    }
                    unsafe { *lr.at(*c as usize) = x };
                    *c += 1;
                });
            }
        }
        })
        .concat();
        // The end of the last node's edges, in its group.
        let g = n >> gb << gb;
        let group = if g == n { m as u32 } else { first.at(g) };
        (first.base[n >> gb], first.rel[n]) = (group, (m as u32 - group) as u8);
        (first, heavy, over.into_inner() || m as u32 - group > 255)
    };
    let (first, heavy, over) = build(3);
    let (first, mut heavy) = if over { (build(0).0, build(0).1) } else { (first, heavy) };
    heavy.sort_unstable();

    // 3. Locality order: compact BFS blobs. A thread grows a blob over
    // unclaimed nodes (claimed by compare-exchange), then starts the next one
    // from the last one's unclaimed frontier, so consecutive ids are close on
    // the map.
    let claimed: Vec<AtomicU8> = (0..n).map(|_| AtomicU8::new(0)).collect();
    let claim = |v: u32| {
        let c = &claimed[v as usize];
        c.load(Relaxed) == 0 && c.compare_exchange(0, 1, Relaxed, Relaxed).is_ok()
    };
    let (first_v, link_v) = (first, link);
    let (first, link) = (&first_v, &link_v[..]);
    // As a thread orders a node it also appends the node's edge words (heads
    // still old ids) to its own region of the new edge array, so the graph is
    // copied into the new order on the way, in sequential writes. Region t
    // starts at t * m in one zeroed reservation, of which only the pages
    // written are ever touched. Returns each chain and its heavy weights.
    assert!(nt * m < 1 << 32, "edge positions must fit 32 bits");
    let mut adj = vec![0u32; nt * m];
    let ar = Raw::of(&mut adj);
    let chains: Vec<(Vec<u32>, Vec<(u32, u32)>)> = on_threads(nt, |t| {
        drop_pages(&filed[t].offs, 0, 1); // the parse output is read no more
        drop_pages(&filed[t].words, 0, 1);
        let (mut ord, mut seeds, mut heavy2) = (Vec::with_capacity(n / nt + BLOB), Vec::new(), Vec::new());
        let (mut scan, mut at) = (share(n, t, nt).start, t * m);
        // Fetch the offsets 8 queue entries ahead and the edges 4 ahead.
        let ahead = |ord: &Vec<u32>, i: usize| {
            ord.get(i + 8).map(|&u| prefetch(&first.rel[u as usize]));
            ord.get(i + 4).map(|&u| prefetch(&link[first.at(u as usize) as usize]));
        };
        // Copies u's edge words to the region; returns them for the caller.
        let mut copy = |u: usize, at: &mut usize| {
            let (lo, hi) = (first.at(u), first.at(u + 1));
            for e in lo..hi {
                let x = link[e as usize];
                if x >> hb == big {
                    heavy2.push((*at as u32, heavy_weight(&heavy, e)));
                }
                unsafe { *ar.at(*at) = x };
                *at += 1;
            }
            &link[lo as usize..hi as usize]
        };
        loop {
            let mut seed = None;
            while let Some(v) = seeds.pop() {
                if claim(v) {
                    seed = Some(v);
                    break;
                }
            }
            while seed.is_none() && scan < n {
                if claim(scan as u32) {
                    seed = Some(scan as u32);
                }
                scan += 1;
            }
            let Some(seed) = seed else { return (ord, heavy2) };
            let (blob, mut i) = (ord.len(), ord.len());
            ord.push(seed);
            while i < ord.len() && ord.len() - blob < BLOB {
                ahead(&ord, i);
                for &x in copy(ord[i] as usize, &mut at) {
                    if ord.len() - blob < BLOB && claim(x & hmask) {
                        ord.push(x & hmask);
                    }
                }
                i += 1;
            }
            for i in i..ord.len() {
                ahead(&ord, i);
                for &x in copy(ord[i] as usize, &mut at) {
                    if claimed[(x & hmask) as usize].load(Relaxed) == 0 {
                        seeds.push(x & hmask);
                    }
                }
            }
        }
    });
    drop(filed);

    // New ids: each thread's chain in turn (every node is in one), then at
    // least one padding id, up to a whole group of 8, so no group of packed
    // offsets spans two regions. Padding ids have no edges, except that the
    // last one's row runs on to the next region; nothing reads it, as no edge
    // leads to a padding id and no query starts there. A node's row starts
    // where the previous one in its chain ends, its degree read off the old
    // offsets.
    let (mut start, lens) = (vec![0; nt + 1], chains.iter().map(|c| c.0.len()).collect::<Vec<_>>());
    for t in 0..nt {
        start[t + 1] = start[t] + (lens[t] + 1).next_multiple_of(8);
    }
    let n2 = start[nt];
    let (mut id_v, mut off_v) = (vec![0u32; n], vec![0u32; n2 + 1]);
    let (ir, or) = (Raw::of(&mut id_v), Raw::of(&mut off_v));
    let ends_at: Vec<usize> = on_threads(nt, |t| {
        // Arrays read no more go back on every thread rather than on one.
        (drop_pages(link, t, nt), drop_pages(&claimed, t, nt));
        let (mut k, mut e) = (start[t], t * m);
        for &u in &chains[t].0 {
            unsafe { (*ir.at(u as usize), *or.at(k)) = (k as u32, e as u32) };
            (k, e) = (k + 1, e + (first.at(u as usize + 1) - first.at(u as usize)) as usize);
        }
        (k..start[t + 1] + (t + 1 == nt) as usize).for_each(|k| unsafe { *or.at(k) = e as u32 });
        e
    });
    drop((claimed, link_v));
    let mut heavy2: Vec<(u32, u32)> = chains.into_iter().flat_map(|c| c.1).collect();
    heavy2.sort_unstable();
    drop((first_v, heavy));
    let off = Offs::pack(nt, &off_v);

    // Each edge's head mapped to its new id (weight bits kept), each thread
    // over its own region, with in-degrees counted into roff[v + 2] on the
    // way. Summed, roff[v + 1] is where v's in-edges start; as v's cursor
    // while they are placed it ends at v's end, the start of v + 1.
    let id = &id_v[..];
    let mut roff_v = vec![0u32; n2 + 2];
    let rr = Raw::of(&mut roff_v);
    let bump = |i: usize| unsafe { AtomicU32::from_ptr(rr.at(i)).fetch_add(1, Relaxed) };
    on_threads(nt, |t| {
        drop_pages(&off_v, t, nt);
        for p in t * m..ends_at[t] {
            if p + 16 < ends_at[t] {
                prefetch(&id[(unsafe { *ar.at(p + 16) } & hmask) as usize]);
            }
            let x = unsafe { *ar.at(p) };
            let v = id[(x & hmask) as usize];
            unsafe { *ar.at(p) = v | (x & !hmask) };
            bump(v as usize + 2);
        }
    });
    // Queries in the new numbering (sorted later), then the id map goes.
    let ends: Vec<(u32, u32)> = queries.iter().map(|&(s, t)| (id[s as usize], id[t as usize])).collect();
    drop(off_v);
    prefix_sum(nt, &mut roff_v);
    let adj = &adj;
    let off = &off;
    let mut radj = vec![0u32; m];
    let rar = Raw::of(&mut radj);
    let mut rheavy: Vec<(u32, u32)> = on_threads(nt, |t| {
        drop_pages(id, t, nt);
        let mut out = Vec::new();
        for u in start[t]..start[t] + lens[t] {
            for e in off.at(u)..off.at(u + 1) {
                let x = adj[e as usize];
                let j = bump((x & hmask) as usize + 1);
                if x >> hb == big {
                    out.push((j, heavy_weight(&heavy2, e)));
                }
                unsafe { *rar.at(j as usize) = u as u32 | (x & !hmask) };
            }
        }
        out
    })
    .concat();
    drop(id_v);
    rheavy.sort_unstable();
    let roff = Offs::pack(nt, &roff_v[..n2 + 1]);

    // 4. Queries sorted so neighbouring searches share cached pages.
    let mut order: Vec<usize> = (0..ends.len()).collect();
    order.sort_unstable_by_key(|&q| ends[q]);
    let graphs = [(off, &adj[..], &heavy2[..], hb), (&roff, &radj[..], &rheavy[..], hb)];
    let mut ans = vec![INF; ends.len()];
    let ansr = Raw::of(&mut ans);
    let next = AtomicUsize::new(0);
    on_threads(nt, |th| {
        drop_pages(&roff_v, th, nt);
        let mut narrow: [Side<u32>; 2] = [Side::new(n2), Side::new(n2)];
        let mut wide: [Side<u64>; 2] = [Side::new(n2), Side::new(n2)];
        loop {
            let q = next.fetch_add(1, Relaxed);
            let Some(&i) = order.get(q) else {
                return drop_pages(text, th, nt); // while the last searches run
            };
            let ((s0, t0), (s, t)) = (queries[i], ends[i]);
            if s0 == t0 {
                unsafe { *ansr.at(i) = 0 };
                continue;
            }
            if roff.at(t as usize + 1) == roff.at(t as usize) {
                continue; // no in-edge: unreachable
            }
            let mut best = search(&mut narrow, graphs, s, t);
            if best.1 && best.0 >= u32::MAX as u64 {
                best = search(&mut wide, graphs, s, t); // the answer may not fit u32
            }
            unsafe { *ansr.at(i) = best.0 };
        }
    });
    drop(roff_v);
    // Free the graph on every thread at once rather than at exit on one.
    on_threads(nt, |t| [adj, &radj].iter().for_each(|b| drop_pages(b, t, nt)));

    // INF prints as -1 (u64::MAX as i64); every real distance is below 2^63.
    let out: String = ans.iter().map(|&a| format!("{}\n", a as i64)).collect();
    std::fs::write("/out/distances.txt", out).unwrap();
}
