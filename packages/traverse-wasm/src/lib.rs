use std::cmp::Ordering;

/// Float wrapper for max-heap ordering
#[derive(Clone, Copy, PartialEq)]
struct OrdF32(f32);
impl Eq for OrdF32 {}
impl Ord for OrdF32 {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.partial_cmp(&other.0).unwrap_or(Ordering::Equal)
    }
}
impl PartialOrd for OrdF32 {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> { Some(self.cmp(other)) }
}

#[derive(Clone)]
struct Entry { ps: OrdF32, node_idx: u32, block_idx: u32 }

/// BFS traversal with static buffer reuse for WASM performance.
#[no_mangle]
pub unsafe extern "C" fn traverse(
    centers: *const f32, total_nodes: u32,
    feature_sizes: *const f32,
    child_starts: *const u32,
    child_counts: *const u16,
    block_offsets: *const u32, num_blocks: u32,
    block_counts: *const u32,
    cam_x: f32, cam_y: f32, cam_z: f32,
    _fwd_x: f32, _fwd_y: f32, _fwd_z: f32,
    lod_scale: f32, pixel_scale_limit: f32,
    max_splats: u32,
    budgets: *const u32,
    out: *mut u32,
) -> u32 {
    let centers = std::slice::from_raw_parts(centers, total_nodes as usize * 3);
    let feature_sizes = std::slice::from_raw_parts(feature_sizes, total_nodes as usize);
    let child_starts = std::slice::from_raw_parts(child_starts, total_nodes as usize);
    let child_counts = std::slice::from_raw_parts(child_counts, total_nodes as usize);
    let block_offsets = std::slice::from_raw_parts(block_offsets, num_blocks as usize);
    let block_counts = std::slice::from_raw_parts(block_counts, num_blocks as usize);
    let budgets = std::slice::from_raw_parts(budgets, num_blocks as usize);
    let out = std::slice::from_raw_parts_mut(out, max_splats as usize);

    if num_blocks == 0 || max_splats == 0 { return 0; }

    // Static buffers reused across calls — no per-frame allocation
    static mut SPENT: Vec<usize> = Vec::new();
    static mut HEAP: Vec<Entry> = Vec::new();

    let spent = unsafe { &mut SPENT };
    spent.clear();
    spent.resize(num_blocks as usize, 0);

    let heap = unsafe { &mut HEAP };
    heap.clear();
    // Reserve capacity once (worst-case heap size)
    if heap.capacity() < 100000 { heap.reserve(100000 - heap.capacity()); }

    // Inline max-heap sift operations on Vec
    macro_rules! heap_push {
        ($e:expr) => {{
            let mut i = heap.len();
            heap.push($e);
            while i > 0 {
                let p = (i - 1) >> 1;
                if heap[p].ps >= heap[i].ps { break; }
                heap.swap(p, i);
                i = p;
            }
        }};
    }
    macro_rules! heap_pop {
        () => {{
            if heap.is_empty() { None } else {
                let last = heap.len() - 1;
                heap.swap(0, last);
                let top = heap.pop();
                let mut i = 0;
                let n = heap.len();
                while i < n {
                    let mut largest = i;
                    let l = (i << 1) | 1;
                    let r = l + 1;
                    if l < n && heap[l].ps > heap[largest].ps { largest = l; }
                    if r < n && heap[r].ps > heap[largest].ps { largest = r; }
                    if largest == i { break; }
                    heap.swap(i, largest);
                    i = largest;
                }
                top
            }
        }};
    }

    let compute_ps = |cx: f32, cy: f32, cz: f32, fs: f32| -> f32 {
        let dx = cx - cam_x; let dy = cy - cam_y; let dz = cz - cam_z;
        let dist = (dx * dx + dy * dy + dz * dz).sqrt().max(1e-6);
        (fs / dist) * lod_scale
    };

    // Push all roots
    for bi in 0..num_blocks as usize {
        let offset = block_offsets[bi] as usize;
        let cnt = block_counts[bi] as usize;
        if cnt == 0 { continue; }
        let ri = offset + cnt - 1;
        let co = ri * 3;
        let ps = compute_ps(centers[co], centers[co+1], centers[co+2], feature_sizes[ri]);
        heap_push!(Entry { ps: OrdF32(ps), node_idx: ri as u32, block_idx: bi as u32 });
    }

    let max_out = max_splats as usize;
    let mut out_count = 0usize;

    while let Some(entry) = heap_pop!() {
        if out_count >= max_out { break; }
        let bidx = entry.block_idx as usize;
        if spent[bidx] >= budgets[bidx] as usize { continue; }
        spent[bidx] += 1;

        let ni = entry.node_idx as usize;
        let cnt = child_counts[ni] as u32;

        if cnt == 0 {
            out[out_count] = entry.node_idx; out_count += 1;
            continue;
        }
        if spent[bidx] + cnt as usize > budgets[bidx] as usize {
            out[out_count] = entry.node_idx; out_count += 1;
            continue;
        }

        let start = child_starts[ni] as usize;
        for c in 0..cnt {
            let ci = start + c as usize;
            let co = ci * 3;
            let ps = compute_ps(centers[co], centers[co+1], centers[co+2], feature_sizes[ci]);
            if ps <= pixel_scale_limit {
                out[out_count] = ci as u32; out_count += 1;
                if out_count >= max_out { break; }
            } else {
                heap_push!(Entry { ps: OrdF32(ps), node_idx: ci as u32, block_idx: entry.block_idx });
            }
        }
    }
    out_count as u32
}
