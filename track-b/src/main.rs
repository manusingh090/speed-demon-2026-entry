//! Track B: answers the 50,000 queries of /data/queries.txt over /data/docs.txt.
//!
//! The corpus is read in phases of about 50 MiB. In each phase every list term's documents
//! become 16-bit postings and every common term a bitmap; the phase answers all queries
//! into a running result per query (count, id sum, first ten ids), and the next phase
//! reuses the space. Phrases are checked while their documents' token hashes are at hand
//! and their matches stored as an extra list term each. All threads run the same worker:
//! parse a part of the queries, sample, plan, then scan, build and answer phase by phase.
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering::*};
use std::sync::{Mutex, OnceLock};
use std::{arch::x86_64::*, fs::File, io::Write, os::fd::AsRawFd, ptr::null_mut, thread};

extern "C" {
    fn mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, offset: i64) -> *mut u8;
    fn madvise(addr: *mut u8, len: usize, advice: i32) -> i32;
    fn prctl(option: i32, arg: usize, a: usize, b: usize, c: usize) -> i32;
    fn syscall(number: i64, ...) -> i64;
    fn memchr(text: *const u8, byte: i32, len: usize) -> *const u8;
}
// mmap flags: private, anonymous, no swap reservation.
const ANONYMOUS: i32 = 0x4022;

const HUGE_PAGE: usize = 1 << 21;
// Bytes of text a worker takes at once.
const SLICE: usize = 1 << 18;
// Slices per phase: postings are rebuilt every this many.
const PHASE_SLICES: usize = 200;
// Postings before each list, holding a value no document has.
const SENTINELS: usize = 1;
const PLAN_PARTS: usize = 16;
const FIRST_IDS: usize = 10;
// Bytes tokenized before the token pass hands over to the term pass.
const TEXT_BATCH: usize = 1 << 14;
// Lines of the term table, and slots in each.
const LINES: usize = 1 << 15;
const LINE_SLOTS: usize = 7;
const ID_RUN: usize = 32;
const MAX_TERMS: usize = LINES * LINE_SLOTS + 64 * ID_RUN;
// Tokens between fetching a table line and reading it.
const AHEAD: usize = 20;
// Action bits: a bitmap term, a full line, a phrase to check.
const BITMAP_FLAG: u32 = 1 << 31;
const SPILL: u32 = 1 << 30;
const CHECK_FLAG: u32 = 1 << 29;
const MAX_BITS: usize = 4096;
// Lists are built in this many independent shards.
const ROW_SHARDS: usize = 64;
const DOC_BITS: u32 = 18;
const ROW_CHUNK: usize = 64;
const JOB_QUERIES: usize = 64;
const WORKER_BYTES: usize = 32 << 20;
const MAX_WORDS: usize = 8;

/// One cache line of the term table: 7 tags, then 8 actions, the last for a miss.
#[repr(C, align(64))]
struct TermLine {
    tags: [u32; LINE_SLOTS + 1],
    actions: [u32; LINE_SLOTS + 1],
}

/// Per-worker buffers for one batch of text.
struct Bench {
    starts: &'static mut [u32],
    ends: &'static mut [u32],
    lines: &'static mut [u32],
    hashes: &'static mut [u64],
    bit_hits: &'static mut [u32],
    bit_ends: &'static mut [u32],
    group: &'static mut [u64],
    phrase_hits: &'static mut [u64],
    acts: &'static mut [u32],
    list_acts: &'static mut [u32],
    list_ends: &'static mut [u32],
    check_hits: &'static mut [u64],
    counts: &'static mut [u32],
}

/// Where a term's documents are in the current phase.
struct TermInfo {
    first: u32,
    count: u32,
    phrases: u32,
    bitmaps_n: bool,
    in_bool: bool,
}

/// A phrase: its words' hashes, its anchor word, and its stand-in term.
struct Window {
    term: u32,
    len: u32,
    pivot: u32,
    next: u32,
    words: [u64; MAX_WORDS],
}

/// A BOOL operator.
#[derive(Clone, Copy, PartialEq)]
enum Op {
    And,
    Or,
    Not,
}

/// A result set: a list of 16-bit document offsets, or a bitmap.
#[derive(Clone, Copy)]
enum Docs {
    List(&'static [u16]),
    Bits(&'static [u64]),
}

/// Per-worker space for intermediate results.
#[derive(Default)]
struct Spare {
    lists: [Vec<u16>; 2],
    bits: Vec<u64>,
    marks: Vec<u64>,
}

/// One query's answer so far, summed over the phases done.
struct Running {
    count: u32,
    sum: u64,
    first: [u32; FIRST_IDS],
}

impl Running {
    fn add_docs(&mut self, base: u32, list: &[u16]) {
        let sum = list.iter().map(|&doc| doc as u64).sum();
        let ids = list.iter().map(|&doc| doc as u32);
        self.add(base, list.len() as u64, sum, ids);
    }

    fn add(&mut self, base: u32, count: u64, sum: u64, ids: impl Iterator<Item = u32>) {
        let room = (self.count as usize).min(FIRST_IDS);
        for (slot, id) in self.first[room..].iter_mut().zip(ids) {
            *slot = base + id;
        }
        self.count += count as u32;
        self.sum += sum + base as u64 * count;
    }

    fn add_bitmap(&mut self, base: u32, bits: &[u64]) {
        let (count, sum) = unsafe {
            let table =
                |row: [i8; 16]| _mm256_broadcastsi128_si256(_mm_loadu_si128(row.as_ptr().cast()));
            let pick = |table, index| _mm256_shuffle_epi8(table, index);
            let ones = table([0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4]);
            let places = table([0, 0, 1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 6, 6]);
            let weights = _mm256_set1_epi64x(0x3830_2820_1810_0800);
            let (low, zero) = (_mm256_set1_epi8(15), _mm256_setzero_si256());
            let (mut counts, mut sums, mut offsets, mut starts) = (zero, zero, zero, zero);
            let mut first = _mm256_setr_epi64x(0, 64, 128, 192);
            for block in bits.chunks_exact(4) {
                let word = _mm256_loadu_si256(block.as_ptr().cast());
                let lo = _mm256_and_si256(word, low);
                let hi = _mm256_and_si256(_mm256_srli_epi16::<4>(word), low);
                let high_ones = pick(ones, hi);
                let per_byte = _mm256_add_epi8(pick(ones, lo), high_ones);
                let low_places = _mm256_add_epi8(pick(places, lo), pick(places, hi));
                let positions = _mm256_add_epi8(low_places, _mm256_slli_epi16::<2>(high_ones));
                let per_word = _mm256_sad_epu8(per_byte, zero);
                counts = _mm256_add_epi64(counts, per_word);
                sums = _mm256_add_epi64(sums, _mm256_sad_epu8(positions, zero));
                starts = _mm256_add_epi64(starts, _mm256_mul_epu32(per_word, first));
                let weighted = _mm256_maddubs_epi16(per_byte, weights);
                offsets =
                    _mm256_add_epi32(offsets, _mm256_madd_epi16(weighted, _mm256_set1_epi16(1)));
                first = _mm256_add_epi64(first, _mm256_set1_epi64x(256));
            }
            let total = |v: __m256i| {
                let lanes: [u32; 8] = std::mem::transmute(v);
                lanes.iter().map(|&lane| lane as u64).sum::<u64>()
            };
            (total(counts), total(sums) + total(starts) + total(offsets))
        };
        let ids = bits
            .iter()
            .enumerate()
            .filter(|(_, &word)| word != 0)
            .flat_map(|(index, &word)| {
                (0..64)
                    .filter(move |bit| word >> bit & 1 == 1)
                    .map(move |bit| index as u32 * 64 + bit)
            });
        self.add(base, count, sum, ids);
    }

    fn write(&self, out: &mut Vec<u8>) {
        push_num(out, self.count as u64);
        push_num(out, self.sum);
        for &doc in &self.first[..(self.count as usize).min(FIRST_IDS)] {
            push_num(out, doc as u64);
        }
        *out.last_mut().unwrap() = b'\n';
    }
}

/// Everything the workers share.
struct Engine {
    text: &'static [u8],
    docs: usize,
    bit_words: usize,
    buckets: *mut TermLine,
    terms: *mut TermInfo,
    term_count: AtomicUsize,
    queries: &'static [u8],
    shares: Vec<OnceLock<QueryPart>>,
    phrases: *mut Window,
    sample: &'static [AtomicU32],
    prep: [AtomicUsize; 6],
    phase: Vec<[AtomicUsize; 4]>,
    phase_at: Vec<u32>,
    waiting: AtomicU32,
    epoch: AtomicU32,
    bitmaps: *mut u64,
    rows: *mut u32,
    shard_fill: Vec<AtomicUsize>,
    postings: *mut u16,
    outputs: Vec<Mutex<Vec<u8>>>,
    arena: *mut u8,
}
unsafe impl Sync for Engine {}

/// Address space on a 2 MiB boundary, huge pages on or off.
unsafe fn map_region<T>(count: usize, huge: bool) -> *mut T {
    let bytes = count * size_of::<T>();
    let region = mmap(null_mut(), bytes + HUGE_PAGE, 3, ANONYMOUS, -1, 0);
    let aligned = region.add(region.align_offset(HUGE_PAGE));
    madvise(aligned, bytes, 15 - huge as i32);
    aligned.cast()
}

/// Returns pages to the kernel.
unsafe fn release(at: *const u8, len: usize) {
    madvise(at as *mut u8, len, 4);
}

/// A slice whose pages exist only once written.
unsafe fn lazy_slice<T>(count: usize) -> &'static mut [T] {
    std::slice::from_raw_parts_mut(map_region(count, false), count)
}

/// The next part of a worker's arena.
unsafe fn carve<T>(next: &mut *mut u8, count: usize) -> &'static mut [T] {
    let at = *next;
    *next = at.add((count * size_of::<T>()).next_multiple_of(4096));
    std::slice::from_raw_parts_mut(at.cast(), count)
}

/// Maps an input file, with zero pages after it for over-reads.
unsafe fn map_input(path: &str) -> &'static [u8] {
    let file = File::open(path).unwrap();
    let len = file.metadata().unwrap().len() as usize;
    let span = len.div_ceil(4096) * 4096 + 4096;
    let base = mmap(null_mut(), span, 1, ANONYMOUS, -1, 0);
    mmap(base, len, 1, 0x12, file.as_raw_fd(), 0);
    std::slice::from_raw_parts(base, len)
}

// Round key for the token hash (47 of its bits tell terms apart).
const AES_KEY: i64 = 0x9E37_79B9_7F4A_7C15_u64 as i64;

/// Lane orders that pack the set lanes of an 8-bit mask to the front.
static LEFT_PACK: [[u8; 8]; 256] = {
    let mut table = [[0u8; 8]; 256];
    let mut mask = 0;
    while mask < 256 {
        let (mut lane, mut n) = (0, 0);
        while lane < 8 {
            if mask >> lane & 1 == 1 {
                table[mask][n] = lane as u8;
                n += 1;
            }
            lane += 1;
        }
        mask += 1;
    }
    table
};

/// Read at 16 - len: the first len bytes set.
static BYTE_MASKS: [[u8; 16]; 2] = [[255; 16], [0; 16]];

/// Hashes two tokens of up to 16 bytes, lowercased, in one pair of AES rounds.
#[inline(always)]
unsafe fn hash_two(a: *const u8, len_a: usize, b: *const u8, len_b: usize) -> (u64, u64) {
    let load = |at: *const u8| _mm_loadu_si128(at.cast());
    let mask_of = |len: usize| load(BYTE_MASKS.as_flattened().as_ptr().add(16 - len));
    let pair = _mm256_set_m128i(load(b), load(a));
    let mask = _mm256_set_m128i(mask_of(len_b), mask_of(len_a));
    let key = _mm256_and_si256(_mm256_or_si256(pair, _mm256_set1_epi8(0x20)), mask);
    let mixed = _mm256_aesenc_epi128(_mm256_aesenc_epi128(key, _mm256_set1_epi64x(AES_KEY)), key);
    let folded = _mm256_xor_si256(mixed, _mm256_srli_si256::<8>(mixed));
    let [low, high] = [
        _mm256_extract_epi64::<0>(folded),
        _mm256_extract_epi64::<2>(folded),
    ];
    (low as u64 | 1, high as u64 | 1)
}

/// Hashes any token.
#[inline(always)]
unsafe fn token_hash(token: *const u8, len: usize) -> u64 {
    if len > 16 {
        hash_long(token, len)
    } else {
        hash_two(token, len, token, len).0
    }
}

/// The scan's action for a token: one line read, no branch on hit or miss.
#[inline(always)]
unsafe fn lookup(buckets: *const TermLine, hash: u64) -> u32 {
    let bucket = buckets.add(line_index(hash));
    let wanted = _mm256_set1_epi32(hash as i32);
    let same = _mm256_cmpeq_epi32(_mm256_load_si256(bucket.cast()), wanted);
    let found = _mm256_movemask_ps(_mm256_castsi256_ps(same)) as u32 | 1 << LINE_SLOTS;
    let action = (*bucket).actions[found.trailing_zeros() as usize];
    if action == SPILL {
        return Engine::lookup_far(buckets, hash);
    }
    action
}

/// The table line a hash belongs to.
#[inline(always)]
fn line_index(hash: u64) -> usize {
    (hash >> (64 - LINES.ilog2())) as usize
}

/// Tokens over 16 bytes: two hashes mixed.
#[cold]
unsafe fn hash_long(token: *const u8, len: usize) -> u64 {
    (token_hash(token, 16).rotate_left(29) ^ token_hash(token.add(16), len - 16)) | 1
}

impl Engine {
    /// The rest of the probe, past full lines.
    #[cold]
    unsafe fn lookup_far(buckets: *const TermLine, hash: u64) -> u32 {
        let mut index = line_index(hash);
        loop {
            let bucket = &*buckets.add(index);
            match bucket.tags.iter().position(|&tag| tag == hash as u32) {
                Some(slot) => return bucket.actions[slot],
                None if bucket.actions[LINE_SLOTS] == 0 => return 0,
                None => index = (index + 1) % LINES,
            }
        }
    }

    /// A fresh chunk of rows in a shard, tagged with its slice.
    #[cold]
    unsafe fn new_chunk(&self, shard: usize, claim: usize) -> (*mut u32, *mut u32) {
        let offset = self.shard_fill[shard].fetch_add(ROW_CHUNK, Relaxed);
        let at = self.rows.add(shard * (self.text.len() + 4096) + offset);
        *at = claim as u32 + 1;
        (at.add(1), at.add(ROW_CHUNK))
    }

    /// Tokenizes a slice, looks up its tokens, checks phrases and writes rows and bits.
    unsafe fn scan_claim<const SAMPLE: bool>(&self, claim: usize, b: &mut Bench) -> usize {
        let (start, mut doc) = line_at_64(self.text, self.docs, claim * SLICE);
        let end = line_at_64(self.text, self.docs, claim * SLICE + SLICE).0;
        if start >= end {
            return 0;
        }
        let (first_doc, text) = (doc, self.text.as_ptr());

        let mut cursors = [(null_mut(), null_mut()); ROW_SHARDS];
        let (mut token_count, mut end_count, mut line_count) = (0, 0, 0);
        let (mut at, mut carry, mut first_token) = (start, 0u64, true);

        let byte = |value: u8| _mm256_set1_epi8(value as i8);
        let (floor_lower, ceil_lower) = (byte(b'a' - 1), byte(b'z' + 1));
        let (floor_digit, ceil_digit) = (byte(b'0' - 1), byte(b'9' + 1));
        let (fold, newline) = (byte(0x20), byte(b'\n'));
        let between = |x, floor, ceil| {
            _mm256_and_si256(_mm256_cmpgt_epi8(x, floor), _mm256_cmpgt_epi8(ceil, x))
        };

        while at < end {
            let batch_end = (at + TEXT_BATCH).min(end);
            while at < batch_end {
                let classify = |at: usize| {
                    let chunk = _mm256_loadu_si256(text.add(at).cast());
                    let lower = _mm256_or_si256(chunk, fold);
                    let letters = between(lower, floor_lower, ceil_lower);
                    let digits = between(chunk, floor_digit, ceil_digit);
                    (
                        _mm256_movemask_epi8(_mm256_or_si256(letters, digits)) as u32 as u64,
                        _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, newline)) as u32 as u64,
                    )
                };
                let (inside_low, breaks_low) = classify(at);
                let (inside_high, breaks_high) = classify(at + 32);
                let mut inside = inside_low | inside_high << 32;
                let mut breaks = breaks_low | breaks_high << 32;
                if at + 64 > end {
                    let keep = (1u64 << (end - at)).wrapping_sub(1);
                    inside &= keep;
                    breaks &= keep;
                }
                let shifted = inside << 1 | carry;
                carry = inside >> 63;
                let mut opened = inside & !shifted;
                let mut closed = !inside & shifted;
                while breaks != 0 {
                    let before = opened & ((1u64 << breaks.trailing_zeros()) - 1);
                    b.lines[line_count] = (token_count + before.count_ones() as usize) as u32;
                    line_count += 1;
                    breaks &= breaks - 1;
                }
                let (found, ended) = (opened.count_ones() as usize, closed.count_ones() as usize);
                let first_in = |bits: u64| (at + bits.trailing_zeros() as usize) as u32;
                for extra in 0..12 {
                    *b.starts.get_unchecked_mut(token_count + extra) = first_in(opened);
                    *b.ends.get_unchecked_mut(end_count + extra) = first_in(closed);
                    opened &= opened.wrapping_sub(1);
                    closed &= closed.wrapping_sub(1);
                }
                for extra in 12..found {
                    b.starts[token_count + extra] = first_in(opened);
                    opened &= opened - 1;
                }
                for extra in 12..ended {
                    b.ends[end_count + extra] = first_in(closed);
                    closed &= closed - 1;
                }
                token_count += found;
                end_count += ended;
                at += 64;
            }

            let ready = b.lines[line_count.saturating_sub(1)] as usize;
            let mut token = 0;
            if first_token && ready > 0 {
                first_token = false;
                token = 1;
                b.hashes[0] = 0;
            }
            let (starts, ends, hashes) =
                (b.starts.as_ptr(), b.ends.as_ptr(), b.hashes.as_mut_ptr());
            let span = |i: usize| (*starts.add(i) as usize, *ends.add(i) as usize);
            let mut i = token;
            while i + 1 < ready {
                let (from_a, to_a) = span(i);
                let (from_b, to_b) = span(i + 1);
                let (len_a, len_b) = (to_a - from_a, to_b - from_b);
                let (a, b) = (text.add(from_a), text.add(from_b));
                (*hashes.add(i), *hashes.add(i + 1)) = if (len_a | len_b) > 16 {
                    (token_hash(a, len_a), token_hash(b, len_b))
                } else {
                    hash_two(a, len_a, b, len_b)
                };
                i += 2;
            }
            if i < ready {
                let (from, to) = span(i);
                *hashes.add(i) = token_hash(text.add(from), to - from);
            }
            let (mut heavy_count, mut list_count, mut anchor_count) = (0, 0, 0);
            let batch_doc = doc;
            let buckets = self.buckets;
            let (bit_hits, list_acts) = (b.bit_hits.as_mut_ptr(), b.list_acts.as_mut_ptr());
            let acts = b.acts.as_mut_ptr();
            for line in 0..line_count {
                let to = b.lines[line] as usize;
                let from = token;
                for t in from..to {
                    let ahead = line_index(*hashes.add(t + AHEAD));
                    _mm_prefetch::<_MM_HINT_T0>(buckets.add(ahead).cast());
                    *acts.add(t) = lookup(buckets, *hashes.add(t));
                }
                let mut t = from;
                while t < to {
                    let v = _mm256_loadu_si256(acts.add(t).cast());
                    let valid = if to - t >= 8 {
                        255
                    } else {
                        (1u32 << (to - t)) - 1
                    };
                    let bitmaps_n = _mm256_movemask_ps(_mm256_castsi256_ps(v)) as u32 & valid;
                    let list = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(
                        v,
                        _mm256_setzero_si256(),
                    ))) as u32
                        & valid;
                    let pick = |mask: u32| {
                        _mm256_cvtepu8_epi32(_mm_loadl_epi64(
                            LEFT_PACK[mask as usize].as_ptr().cast(),
                        ))
                    };
                    _mm256_storeu_si256(
                        bit_hits.add(heavy_count).cast(),
                        _mm256_permutevar8x32_epi32(v, pick(bitmaps_n)),
                    );
                    heavy_count += bitmaps_n.count_ones() as usize;
                    _mm256_storeu_si256(
                        list_acts.add(list_count).cast(),
                        _mm256_permutevar8x32_epi32(v, pick(list)),
                    );
                    list_count += list.count_ones() as usize;
                    let tagged = _mm256_and_si256(v, _mm256_set1_epi32(CHECK_FLAG as i32));
                    if _mm256_testz_si256(tagged, tagged) == 0 {
                        for u in t..(t + 8).min(to) {
                            let action = *acts.add(u);
                            if action & CHECK_FLAG != 0 {
                                *b.check_hits.get_unchecked_mut(anchor_count) =
                                    (action as u64) << 40 | (doc as u64) << 20 | u as u64;
                                anchor_count += 1;
                            }
                        }
                    }
                    t += 8;
                }
                token = to;
                *b.bit_ends.get_unchecked_mut(line) = heavy_count as u32;
                *b.list_ends.get_unchecked_mut(line) = list_count as u32;
                if line + 1 < line_count {
                    doc += 1;
                    b.hashes[to] = 0;
                    token = to + 1;
                }
            }
            let mut phrase_count = 0;
            for (i, &hit) in b.check_hits[..anchor_count].iter().enumerate() {
                let later = |ahead: usize| (b.check_hits[i + ahead] >> 40) as usize;
                _mm_prefetch::<_MM_HINT_T0>(self.terms.add(later(8).wrapping_sub(1)).cast());
                if let Some(term) = later(4).checked_sub(1) {
                    let first = (*self.terms.add(term)).phrases as usize;
                    _mm_prefetch::<_MM_HINT_T0>(self.phrases.add(first.wrapping_sub(1)).cast());
                }
                let token = hit as usize & ((1 << 20) - 1);
                let mut next = (*self.terms.add((hit >> 40) as usize - 1)).phrases;
                while next != 0 {
                    let phrase = &*self.phrases.add(next as usize - 1);
                    next = phrase.next;
                    let (len, pivot) = (phrase.len as usize, phrase.pivot as usize);
                    if token >= pivot
                        && token - pivot + len <= ready
                        && b.hashes[token - pivot..token - pivot + len] == phrase.words[..len]
                    {
                        b.phrase_hits[phrase_count] =
                            (phrase.term as u64 + 1) << 32 | (hit >> 20 & ((1 << 20) - 1));
                        phrase_count += 1;
                    }
                }
            }
            if !SAMPLE {
                let (words, mut from) = (self.bitmaps_n().div_ceil(64), 0);
                for line in 0..line_count {
                    let to = b.bit_ends[line] as usize;
                    let rel = (batch_doc as usize + line
                        - self.phase_at[claim / PHASE_SLICES] as usize)
                        % 64;
                    let row = &mut b.group[rel * words..(rel + 1) * words];
                    for &action in &b.bit_hits[from..to] {
                        let id = (action & !BITMAP_FLAG) as usize;
                        row[id / 64] |= 1 << (id % 64);
                    }
                    from = to;
                    if rel == 63 {
                        self.flush_rows(b.group, claim, batch_doc as usize + line);
                    }
                }
            }
            let mut emit = |hit: u64| {
                let term = ((hit >> 32) as u32 & !CHECK_FLAG) - 1;
                if SAMPLE {
                    b.counts[term as usize] += 1;
                    return;
                }
                let shard = term as usize & (ROW_SHARDS - 1);
                let cursor = cursors.get_unchecked_mut(shard);
                if cursor.0 == cursor.1 {
                    *cursor = self.new_chunk(shard, claim);
                }
                *cursor.0 = ((term >> ROW_SHARDS.trailing_zeros()) + 1) << DOC_BITS | hit as u32;
                cursor.0 = cursor.0.add(1);
            };
            let mut from = 0;
            for line in 0..line_count {
                let to = b.list_ends[line] as usize;
                let doc = (batch_doc as usize + line) as u64;
                for &action in &b.list_acts[from..to] {
                    emit((action as u64) << 32 | doc);
                }
                from = to;
            }
            for &hit in &b.phrase_hits[..phrase_count] {
                emit(hit);
            }
            b.lines[0] = 0;
            line_count = line_count.min(1);
            b.starts.copy_within(ready..token_count, 0);
            token_count -= ready;
            b.ends.copy_within(ready..end_count, 0);
            end_count -= ready;
        }
        if !SAMPLE {
            self.flush_rows(b.group, claim, doc as usize);
        }
        doc - first_doc + 1
    }

    /// Terms that have a bitmap.
    fn bitmaps_n(&self) -> usize {
        self.prep[3].load(Relaxed).min(MAX_BITS)
    }

    /// Moves 64 documents' bitmap rows into the bitmaps, one 64x64 transpose per word.
    unsafe fn flush_rows(&self, group: &mut [u64], claim: usize, doc: usize) {
        let phase = claim / PHASE_SLICES;
        let rel = doc - self.phase_at[phase] as usize;
        let (bitmaps_n, out) = (self.bitmaps_n(), self.bitmap_at(0, phase).add(rel / 64));
        let words = bitmaps_n.div_ceil(64);
        for k in 0..words {
            let mut m: [u64; 64] =
                std::array::from_fn(|j| std::mem::take(&mut group[j * words + k]));
            for width in [32usize, 16, 8, 4, 2, 1] {
                let mask = u64::MAX / ((1 << width) + 1);
                for r in (0..64).filter(|r| r & width == 0) {
                    let t = ((m[r] >> width) ^ m[r + width]) & mask;
                    m[r + width] ^= t;
                    m[r] ^= t << width;
                }
            }
            for (t, &bits) in m.iter().enumerate().take(bitmaps_n - k * 64) {
                *out.add((k * 64 + t) * self.bit_words) |= bits;
            }
        }
    }

    /// Waits for every worker: spin briefly, then sleep on a futex.
    fn sync_all(&self) {
        let epoch = self.epoch.load(Acquire);
        if self.waiting.fetch_add(1, AcqRel) as usize + 1 == self.shares.len() {
            self.waiting.store(0, Relaxed);
            self.epoch.fetch_add(1, Release);
            unsafe { syscall(202, self.epoch.as_ptr(), 129, i32::MAX) };
            return;
        }
        for _ in 0..3000 {
            if self.epoch.load(Acquire) != epoch {
                return;
            }
            std::hint::spin_loop();
        }
        while self.epoch.load(Acquire) == epoch {
            unsafe { syscall(202, self.epoch.as_ptr(), 128, epoch, 0) };
        }
    }

    /// Runs jobs from a shared counter until it passes count.
    fn take_jobs(&self, counter: &AtomicUsize, count: usize, job: impl FnMut(usize)) {
        (0..)
            .map(|_| counter.fetch_add(1, Relaxed))
            .take_while(|&next| next < count)
            .for_each(job);
    }

    /// Gives each phrase its rarest word in the sample as the word that triggers its check.
    unsafe fn pick_anchors(&self, slice: usize) {
        let count = self.prep[5].load(Relaxed);
        for index in count * slice / PLAN_PARTS..count * (slice + 1) / PLAN_PARTS {
            let phrase = &mut *self.phrases.add(index);
            let terms = phrase.words[..phrase.len as usize].iter();
            let terms = terms.map(|&hash| lookup(self.buckets, hash) as usize - 1);
            let (pivot, term) = (terms.enumerate())
                .min_by_key(|&(_, term)| self.sample[term].load(Relaxed))
                .unwrap();
            let anchor = &mut (*self.terms.add(term)).phrases;
            phrase.pivot = pivot as u32;
            phrase.next = AtomicU32::from_ptr(anchor).swap(index as u32 + 1, Relaxed);
        }
    }

    /// Sets each term's scan action once anchors and bitmaps are chosen.
    unsafe fn set_actions(&self, slice: usize) {
        let docs = self.prep[4].load(Relaxed);
        for index in LINES * slice / PLAN_PARTS..LINES * (slice + 1) / PLAN_PARTS {
            let bucket = &mut *self.buckets.add(index);
            for action in &mut bucket.actions[..LINE_SLOTS] {
                if *action == 0 {
                    continue;
                }
                let id = *action as usize - 1;
                let term = &mut *self.terms.add(id);
                if term.phrases != 0 {
                    *action |= CHECK_FLAG;
                } else if !term.in_bool {
                    *action = 0;
                } else if self.sample[id].load(Relaxed) as usize * 32 >= docs {
                    let bitmaps_n = self.prep[3].fetch_add(1, Relaxed);
                    if bitmaps_n < MAX_BITS {
                        *action = BITMAP_FLAG | bitmaps_n as u32;
                        (term.bitmaps_n, term.first) = (true, bitmaps_n as u32);
                    }
                }
            }
        }
    }

    /// Turns one shard's rows into sorted, deduplicated postings for the phase.
    unsafe fn build_lists(&self, shard: usize, phase: usize) {
        let base = self.phase_at[phase];
        let region = self.rows.add(shard * (self.text.len() + 4096));
        let shard_fill = self.shard_fill[shard].load(Relaxed);
        let mut chunks: Vec<(u32, u32)> = (0..shard_fill)
            .step_by(ROW_CHUNK)
            .map(|at| (*region.add(at), at as u32))
            .collect();
        chunks.sort_unstable();
        let term_count = self.term_count.load(Relaxed);
        let terms = term_count.div_ceil(ROW_SHARDS) + 1;
        for id in (shard..term_count).step_by(ROW_SHARDS) {
            (*self.terms.add(id)).count = 0;
        }
        let mut cursors = vec![0u32; terms];
        each_row(region, &chunks, |low, _| {
            *cursors.get_unchecked_mut(low) += 1
        });
        let lists = cursors.iter().filter(|&&size| size > 0).count();
        let total = cursors.iter().sum::<u32>() as usize + lists * SENTINELS;
        let mut at = self.phase[phase][3].fetch_add(total, Relaxed) as u32;
        for cursor in cursors.iter_mut().filter(|cursor| **cursor > 0) {
            let size = *cursor;
            *cursor = at + SENTINELS as u32;
            *self.postings.add(*cursor as usize - 1) = u16::MAX;
            at += size + SENTINELS as u32;
        }
        let firsts = cursors.clone();
        each_row(region, &chunks, |low, doc| {
            let cursor = cursors.get_unchecked_mut(low);
            let slot = self.postings.add(*cursor as usize);
            *slot = (doc - base) as u16;
            *cursor += (*slot.sub(1) != *slot) as u32;
        });
        for low in (0..terms).filter(|&low| firsts[low] > 0) {
            let info = &mut *self.terms.add(low * ROW_SHARDS + shard);
            info.first = firsts[low] - SENTINELS as u32;
            info.count = cursors[low] - firsts[low];
        }
        std::ptr::write_bytes(region, 0, shard_fill);
        self.shard_fill[shard].store(0, Relaxed);
        let bitmaps_n = self.bitmaps_n();
        let (from, to) = (
            bitmaps_n * shard / ROW_SHARDS,
            bitmaps_n * (shard + 1) / ROW_SHARDS,
        );
        let first = self.bitmap_at(from as u32, phase + 1);
        std::ptr::write_bytes(first, 0, (to - from) * self.bit_words);
    }

    /// Bitmap of a term in a phase; two sets alternate between phases.
    unsafe fn bitmap_at(&self, index: u32, phase: usize) -> *mut u64 {
        let set = (phase & 1) * MAX_BITS + index as usize;
        self.bitmaps.add(set * self.bit_words)
    }

    /// A term's documents in a phase.
    unsafe fn docs_of(&self, term: u32, phase: usize) -> Docs {
        let info = &*self.terms.add(term as usize);
        if info.bitmaps_n {
            let bits = self.bitmap_at(info.first, phase);
            return Docs::Bits(std::slice::from_raw_parts(bits, self.bit_words));
        }
        let docs = self.postings.add(info.first as usize + SENTINELS);
        Docs::List(std::slice::from_raw_parts(docs, info.count as usize))
    }

    /// One query on one phase, left to right.
    unsafe fn eval_bool(&self, steps: &[(Op, u32)], phase: usize, scratch: &mut Spare) -> Docs {
        let mut current = self.docs_of(steps[0].1, phase);
        for &(op, term) in &steps[1..] {
            if op != Op::Or && matches!(current, Docs::List([])) {
                continue;
            }
            current = step(op, current, self.docs_of(term, phase), scratch);
        }
        current
    }

    /// Answers a batch of queries on a phase into their running results.
    unsafe fn answer_batch(&self, (share, first, batch): Job, phase: usize, scratch: &mut Spare) {
        let base = self.phase_at[phase];
        let end = (first + JOB_QUERIES).min(share.queries.len());
        let tallies = share.tallies.as_ptr() as *mut Running;
        let steps_of = |at: usize| &share.steps[share.queries[at].clone()];
        let prefetch = |at: usize, lists: bool| {
            for &(_, term) in (at < end).then(|| steps_of(at)).unwrap_or_default() {
                let info = &*self.terms.add(term as usize);
                if !lists {
                    _mm_prefetch::<_MM_HINT_T0>((info as *const TermInfo).cast());
                } else if !info.bitmaps_n {
                    let list = self.postings.add(info.first as usize);
                    for line in 0..(info.count as usize).div_ceil(32).min(6) {
                        _mm_prefetch::<_MM_HINT_T0>(list.add(line * 32).cast());
                    }
                }
            }
        };
        prefetch(first, false);
        prefetch(first + 1, false);
        for index in first..end {
            prefetch(index + 2, false);
            prefetch(index + 1, true);
            let steps = steps_of(index);
            let tally = &mut *tallies.add(index);
            match self.eval_bool(steps, phase, scratch) {
                Docs::List(list) => tally.add_docs(base, list),
                Docs::Bits(bits) => tally.add_bitmap(base, bits),
            }
        }
        if phase + 1 == self.phase_at.len() - 1 {
            let mut out = Vec::with_capacity(JOB_QUERIES * 64);
            for index in first..end {
                (*tallies.add(index)).write(&mut out);
            }
            *self.outputs[batch].lock().unwrap() = out;
        }
    }

    /// Parses this worker's part of the query file and interns its terms.
    unsafe fn parse_part(&self, k: usize) -> QueryPart {
        let (size, shares) = (self.queries.len(), self.shares.len());
        let start = |k: usize| match k * size / shares {
            0 => 0,
            at => self.queries[at - 1..]
                .iter()
                .position(|&c| c == b'\n')
                .map_or(size, |n| at + n),
        };
        let (mut parsed, mut ids, mut op) = (QueryPart::default(), 0..0, Op::And);
        let (mut at, end, base) = (start(k), start(k + 1), self.queries.as_ptr());
        while at < end {
            let stop = memchr(base.add(at), b'\n' as i32, end - at);
            let len = if stop.is_null() {
                end - at
            } else {
                stop.offset_from(base) as usize - at
            };
            let line = &self.queries[at..at + len];
            at += len + 1;
            if line.is_empty() {
                continue;
            }
            let phrase = line[0] == b'P';
            let body = &line[if phrase { 8 } else { 5 }..line.len() - phrase as usize];
            let (first, mut words, mut count) = (parsed.steps.len(), [0; MAX_WORDS], 0);
            for word in body.split(|&c| c == b' ') {
                match word {
                    b"AND" => op = Op::And,
                    b"OR" => op = Op::Or,
                    b"NOT" => op = Op::Not,
                    _ => {
                        let hash = token_hash(word.as_ptr(), word.len());
                        let term = intern_term(self, &mut ids, hash);
                        if phrase {
                            words[count] = hash;
                            count += 1;
                        } else {
                            (*self.terms.add(term as usize)).in_bool = true;
                            parsed.steps.push((op, term));
                        }
                    }
                }
            }
            if phrase {
                let term = next_id(self, &mut ids);
                parsed.steps.push((Op::And, term));
                let slot = &mut *self.phrases.add(self.prep[5].fetch_add(1, Relaxed));
                (slot.term, slot.len, slot.words) = (term, count as u32, words);
            }
            parsed.queries.push(first..parsed.steps.len());
        }
        parsed.tallies = lazy_slice(parsed.queries.len());
        parsed
    }

    /// The worker: parse, sample, plan, then scan, build and answer each phase.
    unsafe fn run_worker(&self, id: usize) {
        let tokens = (1 << 19) + 32;
        let mut next = self.arena.add(id * WORKER_BYTES);
        let mut buffers = Bench {
            starts: carve(&mut next, tokens),
            ends: carve(&mut next, tokens),
            lines: carve(&mut next, TEXT_BATCH / 2 + 64),
            hashes: carve(&mut next, tokens + AHEAD),
            bit_hits: carve(&mut next, tokens),
            bit_ends: carve(&mut next, TEXT_BATCH / 2 + 64),
            group: carve(&mut next, 64 * MAX_BITS / 64),
            phrase_hits: carve(&mut next, tokens),
            acts: carve(&mut next, tokens + 8),
            list_acts: carve(&mut next, tokens + 8),
            list_ends: carve(&mut next, TEXT_BATCH / 2 + 64),
            check_hits: carve(&mut next, tokens),
            counts: carve(&mut next, MAX_TERMS),
        };
        self.shares[id].set(self.parse_part(id)).ok();
        self.sync_all();
        self.take_jobs(&self.prep[0], 12, |claim| {
            self.prep[4].fetch_add(self.scan_claim::<true>(claim, &mut buffers), Relaxed);
        });
        let terms = self.term_count.load(Relaxed);
        for (total, &part) in self.sample.iter().zip(&buffers.counts[..terms]) {
            total.fetch_add(part, Relaxed);
        }
        release(buffers.counts.as_ptr().cast(), MAX_TERMS * 4);
        self.sync_all();
        self.take_jobs(&self.prep[1], PLAN_PARTS, |slice| self.pick_anchors(slice));
        self.sync_all();
        self.take_jobs(&self.prep[2], PLAN_PARTS, |part| self.set_actions(part));
        self.sync_all();

        let mut units = Vec::new();
        for share in self.shares.iter().map(|share| share.get().unwrap()) {
            for first in (0..share.queries.len()).step_by(JOB_QUERIES) {
                units.push((share, first, units.len()));
            }
        }
        let mut scratch = Spare {
            bits: vec![0; self.bit_words],
            marks: vec![0; self.bit_words],
            ..Default::default()
        };
        let phases = self.phase_at.len() - 1;
        for stage in 0..=phases {
            if stage < phases {
                self.take_jobs(&self.phase[stage][0], PHASE_SLICES, |claim| {
                    self.scan_claim::<false>(stage * PHASE_SLICES + claim, &mut buffers);
                });
            }
            if stage > 0 {
                self.take_jobs(&self.phase[stage - 1][2], units.len(), |batch| {
                    self.answer_batch(units[batch], stage - 1, &mut scratch)
                });
            }
            if stage == phases {
                break;
            }
            self.sync_all();
            if stage + 1 == phases {
                let page =
                    |n: usize| (self.text.len() * n / self.shares.len()).div_ceil(4096) * 4096;
                release(self.text.as_ptr().add(page(id)), page(id + 1) - page(id));
            }
            assert!(self.phase_at[stage + 1] - self.phase_at[stage] < u16::MAX as u32);
            self.take_jobs(&self.phase[stage][1], ROW_SHARDS, |shard| {
                self.build_lists(shard, stage)
            });
            self.sync_all();
        }
    }
}

/// Visits a shard's rows in document order.
#[inline(always)]
unsafe fn each_row(region: *const u32, chunks: &[(u32, u32)], mut visit: impl FnMut(usize, u32)) {
    for &(_, at) in chunks {
        for i in 1..ROW_CHUNK {
            let row = *region.add(at as usize + i);
            if row == 0 {
                break;
            }
            visit((row >> DOC_BITS) as usize - 1, row & ((1 << DOC_BITS) - 1));
        }
    }
}

/// The id at the start of a line.
unsafe fn line_id(line: *const u8) -> usize {
    let digits = (0..).map(|i| *line.add(i)).take_while(|&c| c != b'\t');
    digits.fold(0, |id, digit| id * 10 + (digit - b'0') as usize)
}

/// The first line at or after a position whose id is a multiple of 64.
unsafe fn line_at_64(text: &[u8], docs: usize, at: usize) -> (usize, usize) {
    let (base, len) = (text.as_ptr(), text.len());
    let past_line =
        |from: usize| memchr(base.add(from), b'\n' as i32, len - from) as usize - base as usize + 1;
    let mut line = at;
    if at > 0 && at < len && *base.add(at - 1) != b'\n' {
        line = past_line(at);
    }
    while line < len {
        let id = line_id(base.add(line));
        if id % 64 == 0 {
            return (line, id);
        }
        line = past_line(line);
    }
    (len, docs)
}

/// AND or NOT of two lists: binary search for a far shorter one, else marks.
unsafe fn keep_by(a: &[u16], b: &[u16], keep: bool, out: &mut Vec<u16>, marks: &mut [u64]) {
    out.reserve(a.len().max(b.len()));
    let target = out.as_mut_ptr();
    let mut found = 0;
    if a.len() * 16 <= b.len() {
        for &doc in a {
            let present = b.binary_search(&doc).is_ok();
            *target.add(found) = doc;
            found += (present == keep) as usize;
        }
    } else {
        let (marked, tested) = if keep { (a, b) } else { (b, a) };
        for &doc in marked {
            *marks.get_unchecked_mut(doc as usize / 64) |= 1 << (doc % 64);
        }
        for &doc in tested {
            let present = *marks.get_unchecked(doc as usize / 64) >> (doc % 64) & 1;
            *target.add(found) = doc;
            found += (present == keep as u64) as usize;
        }
        for &doc in marked {
            *marks.get_unchecked_mut(doc as usize / 64) = 0;
        }
    }
    out.set_len(found);
}

/// OR of two lists.
fn merge_or(a: &[u16], b: &[u16], out: &mut Vec<u16>) {
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

/// One step of a query; big results become a bitmap.
unsafe fn step(op: Op, current: Docs, term: Docs, scratch: &mut Spare) -> Docs {
    let swap = match (op, current, term) {
        (Op::And, Docs::Bits(_), Docs::List(_)) => true,
        (Op::And, Docs::List(a), Docs::List(b)) => b.len() < a.len(),
        _ => false,
    };
    let (a, b) = [(current, term), (term, current)][swap as usize];
    scratch.lists.swap(0, 1);
    let out = &mut scratch.lists[0];
    out.clear();
    let bits = &mut scratch.bits;
    let (words, keep, marks) = (bits.len(), op == Op::And, &mut scratch.marks);
    match (op, a, b) {
        (Op::And | Op::Not, Docs::List(a), Docs::List(b)) => keep_by(a, b, keep, out, marks),
        (Op::And | Op::Not, Docs::List(a), Docs::Bits(b)) => {
            let held = |doc: u16| *b.get_unchecked(doc as usize / 64) >> (doc % 64) & 1 == 1;
            out.extend(a.iter().filter(|&&doc| held(doc) == keep));
        }
        (Op::Or, Docs::List(a), Docs::List(b)) if a.len() + b.len() < words => merge_or(a, b, out),
        _ => {
            let mark = |bits: &mut [u64], docs: &[u16], clear: bool| {
                for &doc in docs {
                    let word = bits.get_unchecked_mut(doc as usize / 64);
                    let bit = 1 << (doc % 64);
                    *word = if clear { *word & !bit } else { *word | bit };
                }
            };
            match a {
                Docs::Bits(source) if source.as_ptr() != bits.as_ptr() => {
                    bits.copy_from_slice(source)
                }
                Docs::Bits(_) => {}
                Docs::List(docs) => {
                    bits.fill(0);
                    mark(bits, docs, false);
                }
            }
            match b {
                Docs::List(docs) => mark(bits, docs, op == Op::Not),
                Docs::Bits(other) => {
                    for (word, &other) in bits.iter_mut().zip(other) {
                        *word = match op {
                            Op::And => *word & other,
                            Op::Or => *word | other,
                            Op::Not => *word & !other,
                        };
                    }
                }
            }
            return Docs::Bits(std::slice::from_raw_parts(bits.as_ptr(), words));
        }
    }
    Docs::List(std::slice::from_raw_parts(out.as_ptr(), out.len()))
}

/// Writes a number and a space.
fn push_num(out: &mut Vec<u8>, mut value: u64) {
    let from = out.len();
    while out.len() == from || value > 0 {
        out.push(b'0' + (value % 10) as u8);
        value /= 10;
    }
    out[from..].reverse();
    out.push(b' ');
}

/// Finds or adds a term, from any thread, by compare-exchange on its tag.
unsafe fn intern_term(run: &Engine, ids: &mut std::ops::Range<u32>, hash: u64) -> u32 {
    let mut index = line_index(hash);
    loop {
        let bucket = &mut *run.buckets.add(index);
        for slot in 0..LINE_SLOTS {
            let tag = AtomicU32::from_ptr(&mut bucket.tags[slot]);
            let action = AtomicU32::from_ptr(&mut bucket.actions[slot]);
            let mut seen = tag.load(Acquire);
            if seen == 0 {
                seen = match tag.compare_exchange(0, hash as u32, AcqRel, Acquire) {
                    Ok(_) => {
                        let term = next_id(run, ids);
                        action.store(term + 1, Release);
                        return term;
                    }
                    Err(other) => other,
                };
            }
            if seen == hash as u32 {
                while action.load(Acquire) == 0 {
                    std::hint::spin_loop();
                }
                return action.load(Acquire) - 1;
            }
        }
        AtomicU32::from_ptr(&mut bucket.actions[LINE_SLOTS]).store(SPILL, Relaxed);
        index = (index + 1) % LINES;
    }
}

/// A new term id, taken in runs to spare the shared counter.
unsafe fn next_id(run: &Engine, ids: &mut std::ops::Range<u32>) -> u32 {
    ids.next().unwrap_or_else(|| {
        let first = run.term_count.fetch_add(ID_RUN, Relaxed) as u32;
        *ids = first + 1..first + ID_RUN as u32;
        first
    })
}

/// A batch of queries: their part, the first one, and the output slot.
type Job<'a> = (&'a QueryPart, usize, usize);

/// A worker's parsed queries and their running results.
#[derive(Default)]
struct QueryPart {
    queries: Vec<std::ops::Range<usize>>,
    steps: Vec<(Op, u32)>,
    tallies: &'static mut [Running],
}

/// Starts workers as a binary tree, each spawning two before working.
fn start_tree<'scope, 'env>(
    scope: &'scope thread::Scope<'scope, 'env>,
    run: &'env Engine,
    id: usize,
) {
    for child in (2 * id + 1..2 * id + 3).filter(|&c| c < run.shares.len()) {
        scope.spawn(move || start_tree(scope, run, child));
    }
    unsafe { run.run_worker(id) };
}

/// Maps the inputs, sets up the shared state, runs the workers, writes the file.
fn main() {
    let threads = thread::available_parallelism().map_or(12, |n| n.get());
    unsafe {
        let [text, queries] = ["/data/docs.txt", "/data/queries.txt"].map(|path| map_input(path));
        let last_line = text.rsplit(|&c| c == b'\n').nth(1);
        let docs = last_line.map_or(0, |line| line_id(line.as_ptr()) + 1);
        assert!(docs < 1 << DOC_BITS);
        let phases = text.len().div_ceil(PHASE_SLICES * SLICE).max(1);
        let phase_at: Vec<u32> = (0..=phases)
            .map(|phase| line_at_64(text, docs, phase * PHASE_SLICES * SLICE).1 as u32)
            .collect();
        let widest = phase_at.windows(2).fold(0, |m, w| m.max(w[1] - w[0]));
        let bit_words = (widest as usize / 64 + 2).next_multiple_of(4);
        let batches = queries.len() / (7 * JOB_QUERIES) + threads;
        let run = Engine {
            text,
            docs,
            bit_words,
            buckets: map_region(LINES, true),
            terms: map_region(MAX_TERMS, true),
            term_count: AtomicUsize::new(0),
            queries,
            shares: (0..threads).map(|_| OnceLock::new()).collect(),
            phrases: map_region(MAX_TERMS, false),
            sample: lazy_slice(MAX_TERMS),
            prep: [0; 6].map(AtomicUsize::new),
            phase: (0..phases).map(|_| Default::default()).collect(),
            phase_at,
            waiting: Default::default(),
            epoch: Default::default(),
            bitmaps: map_region(2 * MAX_BITS * bit_words, true),
            rows: map_region(ROW_SHARDS * (text.len() + 4096), false),
            shard_fill: (0..ROW_SHARDS).map(|_| AtomicUsize::new(0)).collect(),
            postings: map_region(text.len() + HUGE_PAGE, true),
            outputs: (0..batches).map(|_| Mutex::default()).collect(),
            arena: map_region(threads * WORKER_BYTES, false),
        };
        run.buckets.cast::<u8>().write_volatile(0);
        prctl(41, 1, 0, 0, 0);

        thread::scope(|s| start_tree(s, &run, 0));

        let outputs = run
            .outputs
            .iter()
            .map(|part| std::mem::take(&mut *part.lock().unwrap()));
        let text: Vec<u8> = outputs.collect::<Vec<_>>().concat();
        File::create("/out/results.txt")
            .unwrap()
            .write_all(&text)
            .unwrap();
    }
}
