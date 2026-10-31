//! Speed Demon 2026, Track D (Traverse), in Rust with no dependencies.
//!
//! 1. Parse once. graph.txt is memory-mapped and cut into one line-aligned
//!    range per thread. Each thread files every edge, as sequential appends,
//!    under the part of the node range its tail falls in (about 27 parts of
//!    65,536 nodes). An edge is one 32-bit word: the head in the low bits, the
//!    weight above it; a weight too large for its field is stored as all ones
//!    with its value on a side list.
//! 2. Build each part on its own. A thread takes a part, counts its nodes'
//!    out-degrees and places its edges, all inside a 256 KB counter array, so
//!    the adjacency list is built with no atomics and no cache misses beyond
//!    the writes themselves.
//! 3. Renumber for locality. Node ids are a random relabelling, so each thread
//!    cuts the road network into compact blobs of 512 nodes grown by BFS, and
//!    the graph is copied into that numbering, then reversed.
//! 4. Bidirectional Dijkstra per query, with radix heaps and distances kept in
//!    pages handed out on first touch. A target with no in-edge is -1 at once.
use std::arch::x86_64::{_mm_prefetch, _MM_HINT_T0};
use std::ffi::c_void;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU32, AtomicU8, AtomicUsize, Ordering::Relaxed};
use std::sync::Mutex;

extern "C" {
    fn mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut c_void;
    fn prctl(op: i32, a: u64, b: u64, c: u64, d: u64) -> i32;
    fn mallopt(param: i32, value: i32) -> i32;
}

const INF: u64 = u64::MAX;
const BLOB: usize = 512; // nodes per locality blob
const PART_BITS: u32 = 16; // nodes per part: 2^16, so a part's counters fit in L2
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

/// Runs f(t) on nt threads and collects what each returns.
fn on_threads<R: Send>(nt: usize, f: impl Fn(usize) -> R + Sync) -> Vec<R> {
    let f = &f;
    std::thread::scope(|s| {
        let hs: Vec<_> = (0..nt).map(|t| s.spawn(move || f(t))).collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    })
}

/// Thread t's share of 0..n.
fn share(n: usize, t: usize, nt: usize) -> std::ops::Range<usize> {
    n * t / nt..n * (t + 1) / nt
}

/// A monotone priority queue: keys never drop below the last one popped, so
/// a key only moves to a lower bucket, and only the lowest is ever split.
struct RadixHeap {
    b: Vec<Vec<(u64, u32)>>,
    len: usize,
    last: u64,
}

impl RadixHeap {
    fn new() -> Self {
        RadixHeap { b: (0..65).map(|_| Vec::new()).collect(), len: 0, last: 0 }
    }
    fn slot(&self, k: u64) -> usize {
        (64 - (k ^ self.last).leading_zeros()) as usize
    }
    fn push(&mut self, k: u64, u: u32) {
        let s = self.slot(k);
        unsafe { self.b.get_unchecked_mut(s) }.push((k, u));
        self.len += 1;
    }
    /// The smallest key (the heap must not be empty).
    fn top(&mut self) -> u64 {
        if self.b[0].is_empty() {
            let i = (1..65).find(|&i| !self.b[i].is_empty()).unwrap();
            let mut v = std::mem::take(&mut self.b[i]);
            self.last = v.iter().map(|e| e.0).min().unwrap();
            for &(k, u) in &v {
                let s = self.slot(k);
                self.b[s].push((k, u));
            }
            v.clear();
            self.b[i] = v;
        }
        self.last
    }
    fn pop(&mut self) -> (u64, u32) {
        self.top();
        self.len -= 1;
        self.b[0].pop().unwrap()
    }
    fn clear(&mut self) {
        self.b.iter_mut().for_each(Vec::clear);
        (self.len, self.last) = (0, 0);
    }
}

/// Tentative distances in pages of 512, handed out on first touch and taken
/// back after each query, so a thread's memory follows its largest search.
struct Dist {
    tab: Vec<u16>,
    used: Vec<u32>,
    pool: Vec<u64>,
}

impl Dist {
    fn new(n: usize) -> Self {
        Dist { tab: vec![0; n / PAGE + 1], used: Vec::new(), pool: Vec::new() }
    }
    /// INF for a node this search never reached; hands out no page.
    fn get(&self, v: u32) -> u64 {
        match unsafe { *self.tab.get_unchecked(v as usize / PAGE) } as usize {
            0 => INF,
            pg => unsafe { *self.pool.get_unchecked((pg - 1) * PAGE + v as usize % PAGE) },
        }
    }
    fn at(&mut self, v: u32) -> &mut u64 {
        let i = v as usize / PAGE;
        if unsafe { *self.tab.get_unchecked(i) } == 0 {
            self.used.push(i as u32);
            self.tab[i] = self.used.len() as u16;
            let s = (self.used.len() - 1) * PAGE;
            self.pool.resize(self.pool.len().max(s + PAGE), INF);
            self.pool[s..s + PAGE].fill(INF);
        }
        let k = (self.tab[i] as usize - 1) * PAGE + v as usize % PAGE;
        unsafe { self.pool.get_unchecked_mut(k) }
    }
    fn reset(&mut self) {
        self.used.iter().for_each(|&i| self.tab[i as usize] = 0);
        self.used.clear();
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
    let hb = 32 - (n as u32 | 1).leading_zeros(); // bits for a node id
    let (hmask, big) = ((1u32 << hb) - 1, u32::MAX >> hb);
    let sh = PART_BITS.min(hb);
    let parts = (n >> sh) + 1;

    // 1. Parse once, filing each edge under its tail's part.
    type Bucket = (Vec<u64>, Vec<u32>); // (offset in part << 32 | word), heavy weights in order
    let cut = cuts(text, i, nt);
    let filed: Vec<Vec<Bucket>> = on_threads(nt, |t| {
        let mut out: Vec<Bucket> = (0..parts).map(|_| Default::default()).collect();
        let guess = (cut[t + 1] - cut[t]) / 17 / parts + 64;
        out.iter_mut().for_each(|b| b.0.reserve(guess));
        let mut i = cut[t];
        while i < cut[t + 1] {
            let u = number(text, &mut i);
            let v = number(text, &mut i);
            let mut w = number(text, &mut i);
            if u == v {
                continue; // a self-loop never shortens a path
            }
            let b = &mut out[(u >> sh) as usize];
            if w >= big {
                b.1.push(w);
                w = big;
            }
            b.0.push(((u & ((1 << sh) - 1)) as u64) << 32 | (v | w << hb) as u64);
        }
        out
    });
    // Part p's edges start where part p - 1's end.
    let mut base = vec![0u32; parts + 1];
    for p in 0..parts {
        base[p + 1] = base[p] + filed.iter().map(|f| f[p].0.len() as u32).sum::<u32>();
    }
    let buckets: Vec<Mutex<Bucket>> = (0..parts * nt).map(|_| Mutex::default()).collect();
    for (t, f) in filed.into_iter().enumerate() {
        for (p, b) in f.into_iter().enumerate() {
            *buckets[p * nt + t].lock().unwrap() = b;
        }
    }

    // 2. Build each part's adjacency on its own.
    let m = base[parts] as usize;
    let (mut first, mut link) = (vec![0u32; n + 1], vec![0u32; m]);
    first[n] = m as u32;
    let (fr, lr) = (Raw::of(&mut first), Raw::of(&mut link));
    let claim_part = AtomicUsize::new(0);
    let mut heavy: Vec<(u32, u32)> = on_threads(nt, |_| {
        let mut heavy = Vec::new();
        loop {
            let p = claim_part.fetch_add(1, Relaxed);
            if p >= parts {
                return heavy;
            }
            let lo = p << sh;
            let len = (n - lo).min(1 << sh);
            let mine: Vec<Bucket> = (0..nt).map(|t| std::mem::take(&mut *buckets[p * nt + t].lock().unwrap())).collect();
            let mut cursor = vec![0u32; len];
            mine.iter().for_each(|b| b.0.iter().for_each(|&r| cursor[(r >> 32) as usize] += 1));
            let mut at = base[p];
            for (k, c) in cursor.iter_mut().enumerate() {
                unsafe { *fr.at(lo + k) = at };
                (at, *c) = (at + *c, at);
            }
            for (records, weights) in &mine {
                let mut h = weights.iter();
                for &r in records {
                    let c = &mut cursor[(r >> 32) as usize];
                    let x = r as u32;
                    if x >> hb == big {
                        heavy.push((*c, *h.next().unwrap()));
                    }
                    unsafe { *lr.at(*c as usize) = x };
                    *c += 1;
                }
            }
        }
    })
    .concat();
    heavy.sort_unstable();
    drop(buckets);

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
    let (first, link) = (&first_v[..], &link_v[..]);
    let chains: Vec<Vec<u32>> = on_threads(nt, |t| {
        let (mut ord, mut seeds) = (Vec::with_capacity(n / nt + BLOB), Vec::new());
        let mut scan = share(n, t, nt).start;
        // Fetch the offsets 8 queue entries ahead and the edges 4 ahead.
        let ahead = |ord: &Vec<u32>, i: usize| {
            ord.get(i + 8).map(|&u| prefetch(&first[u as usize]));
            ord.get(i + 4).map(|&u| prefetch(&link[first[u as usize] as usize]));
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
                if first[scan + 1] > first[scan] && claim(scan as u32) {
                    seed = Some(scan as u32);
                }
                scan += 1;
            }
            let Some(seed) = seed else { return ord };
            let (blob, mut i) = (ord.len(), ord.len());
            ord.push(seed);
            while i < ord.len() && ord.len() - blob < BLOB {
                ahead(&ord, i);
                let u = ord[i] as usize;
                for e in first[u]..first[u + 1] {
                    let v = link[e as usize] & hmask;
                    if ord.len() - blob < BLOB && claim(v) {
                        ord.push(v);
                    }
                }
                i += 1;
            }
            for i in i..ord.len() {
                ahead(&ord, i);
                let u = ord[i] as usize;
                for e in first[u]..first[u + 1] {
                    let v = link[e as usize] & hmask;
                    if claimed[v as usize].load(Relaxed) == 0 {
                        seeds.push(v);
                    }
                }
            }
        }
    });

    // New ids: each thread's chain in turn, then the nodes no blob reached.
    let mut id_v = vec![0u32; n];
    let mut start = vec![0u32; nt + 1];
    for t in 0..nt {
        start[t + 1] = start[t] + chains[t].len() as u32;
    }
    let ir = Raw::of(&mut id_v);
    on_threads(nt, |t| chains[t].iter().zip(start[t]..).for_each(|(&u, k)| unsafe { *ir.at(u as usize) = k }));
    let mut k = start[nt];
    for u in 0..n {
        if claimed[u].load(Relaxed) == 0 {
            (id_v[u], k) = (k, k + 1);
        }
    }
    drop((claimed, chains));

    // The graph in the new numbering: degrees scattered, summed, then each
    // edge's head mapped (weight bits kept), with the random reads of later
    // nodes started 8 and 4 nodes early.
    let id = &id_v[..];
    let mut off = vec![0u32; n + 1];
    let or = Raw::of(&mut off);
    on_threads(nt, |t| share(n, t, nt).for_each(|u| unsafe { *or.at(id[u] as usize + 1) = first[u + 1] - first[u] }));
    for v in 0..n {
        off[v + 1] += off[v];
    }
    let mut adj = vec![0u32; m];
    let ar = Raw::of(&mut adj);
    let off = &off;
    let mut heavy2: Vec<(u32, u32)> = on_threads(nt, |t| {
        let (mut out, r) = (Vec::new(), share(n, t, nt));
        for u in r.clone() {
            if u + 8 < r.end {
                prefetch(&off[id[u + 8] as usize]);
                (first[u + 8]..first[u + 9]).for_each(|e| prefetch(&id[(link[e as usize] & hmask) as usize]));
            }
            if u + 4 < r.end {
                prefetch(unsafe { ar.at(off[id[u + 4] as usize] as usize) });
            }
            let mut j = off[id[u] as usize];
            for e in first[u]..first[u + 1] {
                let x = link[e as usize];
                if x >> hb == big {
                    out.push((j, heavy_weight(&heavy, e)));
                }
                unsafe { *ar.at(j as usize) = id[(x & hmask) as usize] | (x & !hmask) };
                j += 1;
            }
        }
        out
    })
    .concat();
    heavy2.sort_unstable();
    // Queries in the new numbering (sorted later), then the old graph and the
    // id map can go.
    let ends: Vec<(u32, u32)> = queries.iter().map(|&(s, t)| (id[s as usize], id[t as usize])).collect();
    drop((heavy, first_v, link_v, id_v));

    // The reverse graph in the same format: in-degrees counted into
    // roff[v + 1], summed, then roff[v] used as v's cursor, which leaves it at
    // v's end, so shifting it up by one restores the offsets.
    let adj = &adj;
    let mut roff = vec![0u32; n + 1];
    let rr = Raw::of(&mut roff);
    let bump = |i: usize| unsafe { AtomicU32::from_ptr(rr.at(i)).fetch_add(1, Relaxed) };
    on_threads(nt, |t| share(n, t, nt).for_each(|u| (off[u]..off[u + 1]).for_each(|e| { bump((adj[e as usize] & hmask) as usize + 1); })));
    for v in 0..n {
        roff[v + 1] += roff[v];
    }
    let mut radj = vec![0u32; m];
    let rar = Raw::of(&mut radj);
    let mut rheavy: Vec<(u32, u32)> = on_threads(nt, |t| {
        let mut out = Vec::new();
        for u in share(n, t, nt) {
            for e in off[u]..off[u + 1] {
                let x = adj[e as usize];
                let j = bump((x & hmask) as usize);
                if x >> hb == big {
                    out.push((j, heavy_weight(&heavy2, e)));
                }
                unsafe { *rar.at(j as usize) = u as u32 | (x & !hmask) };
            }
        }
        out
    })
    .concat();
    rheavy.sort_unstable();
    roff.copy_within(0..n, 1);
    roff[0] = 0;

    // 4. Queries sorted so neighbouring searches share cached pages.
    let mut order: Vec<usize> = (0..ends.len()).collect();
    order.sort_unstable_by_key(|&q| ends[q]);
    // Each direction: offsets, packed edge words, heavy weights.
    let sides = [(&off[..], &adj[..], &heavy2[..]), (&roff[..], &radj[..], &rheavy[..])];
    let mut ans = vec![INF; ends.len()];
    let ansr = Raw::of(&mut ans);
    let next = AtomicUsize::new(0);
    // Bidirectional Dijkstra: grow the side with the smaller queue, record the
    // best length where the sides meet, and stop once the two smallest queued
    // keys add up to at least that. A side that runs dry has reached every node
    // it can, so the best meeting seen is the answer.
    on_threads(nt, |_| {
        let mut dist = [Dist::new(n), Dist::new(n)];
        let mut queue = [RadixHeap::new(), RadixHeap::new()];
        loop {
            let q = next.fetch_add(1, Relaxed);
            let Some(&i) = order.get(q) else { return };
            let ((s0, t0), (s, t)) = (queries[i], ends[i]);
            if s0 == t0 {
                unsafe { *ansr.at(i) = 0 };
                continue;
            }
            if roff[t as usize + 1] == roff[t as usize] {
                continue; // no in-edge: unreachable
            }
            let mut best = INF;
            *dist[0].at(s) = 0;
            queue[0].push(0, s);
            *dist[1].at(t) = 0;
            queue[1].push(0, t);
            while queue[0].len > 0 && queue[1].len > 0 && queue[0].top() + queue[1].top() < best {
                let k = (queue[1].len < queue[0].len) as usize;
                let (go, ga, gh) = sides[k];
                let (a, b) = dist.split_at_mut(1);
                let (mine, other) = if k == 0 { (a, b) } else { (b, a) };
                let (d, u) = queue[k].pop();
                if d > mine[0].get(u) {
                    continue;
                }
                let lo = unsafe { *go.get_unchecked(u as usize) };
                let hi = unsafe { *go.get_unchecked(u as usize + 1) };
                for e in lo..hi {
                    let x = unsafe { *ga.get_unchecked(e as usize) };
                    let (v, w) = (x & hmask, x >> hb);
                    let w = if w == big { heavy_weight(gh, e) } else { w };
                    let nd = d + w as u64;
                    if nd < best {
                        let slot = mine[0].at(v);
                        if nd < *slot {
                            *slot = nd;
                            queue[k].push(nd, v);
                            prefetch(go.as_ptr().wrapping_add(v as usize));
                            best = best.min(nd.saturating_add(other[0].get(v)));
                        }
                    }
                }
            }
            unsafe { *ansr.at(i) = best };
            queue.iter_mut().for_each(RadixHeap::clear);
            dist.iter_mut().for_each(Dist::reset);
        }
    });

    // INF prints as -1 (u64::MAX as i64); every real distance is below 2^63.
    let out: String = ans.iter().map(|&a| format!("{}\n", a as i64)).collect();
    std::fs::write("/out/distances.txt", out).unwrap();
}
