use std::collections::BTreeMap;

use crate::appkernel;
use crate::error::BackendError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StagedFragment {
    pub seq: u32,
    pub disk_offset_bytes: u64,
    pub ordinal: u64,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct StagedWriteRecord {
    pub total_seq: u32,
    pub fragments: BTreeMap<u32, StagedFragment>,
}

#[derive(Debug, Default)]
pub struct StagingStore {
    writes: BTreeMap<u64, StagedWriteRecord>,
    next_ordinal: u64,
}

impl StagingStore {
    pub fn new() -> Self {
        Self {
            writes: BTreeMap::new(),
            next_ordinal: 1,
        }
    }

    pub fn stage_write_locked(
        &mut self,
        op: &appkernel::AkWriteOp,
        data_buffer: &[u8],
        disk_size_bytes: u64,
    ) -> appkernel::AkStatus {
        if data_buffer.len() != op.data_length as usize {
            return appkernel::AK_STATUS_INVALID_PARAMETER;
        }

        let write_begin = op.offset_bytes;
        let write_end = write_begin.saturating_add(u64::from(op.data_length));
        if write_end < write_begin || write_end > disk_size_bytes {
            return appkernel::AK_STATUS_INVALID_PARAMETER;
        }

        let record = self.writes.entry(op.event_id).or_default();
        if record.total_seq != 0 && record.total_seq != op.total_seq {
            return appkernel::AK_STATUS_INVALID_PARAMETER;
        }

        record.total_seq = op.total_seq;
        let fragment = StagedFragment {
            seq: op.seq,
            disk_offset_bytes: op.offset_bytes,
            ordinal: self.next_ordinal,
            data: data_buffer.to_vec(),
        };
        self.next_ordinal += 1;
        record.fragments.insert(op.seq, fragment);
        appkernel::AK_STATUS_SUCCESS
    }

    pub fn overlay_read_locked(&self, request_begin: u64, buffer: &mut [u8]) {
        #[derive(Clone, Copy)]
        struct OverlaySlice<'a> {
            ordinal: u64,
            dest_offset: usize,
            source_offset: usize,
            length: usize,
            data: &'a [u8],
        }

        if buffer.is_empty() {
            return;
        }

        let request_end = request_begin.saturating_add(buffer.len() as u64);
        let mut overlays = Vec::new();

        for staged_entry in self.writes.values() {
            for fragment in staged_entry.fragments.values() {
                let fragment_begin = fragment.disk_offset_bytes;
                let fragment_end = fragment_begin.saturating_add(fragment.data.len() as u64);
                if fragment_end <= request_begin || fragment_begin >= request_end {
                    continue;
                }

                let overlap_begin = fragment_begin.max(request_begin);
                let overlap_end = fragment_end.min(request_end);
                if overlap_end <= overlap_begin {
                    continue;
                }

                overlays.push(OverlaySlice {
                    ordinal: fragment.ordinal,
                    dest_offset: (overlap_begin - request_begin) as usize,
                    source_offset: (overlap_begin - fragment_begin) as usize,
                    length: (overlap_end - overlap_begin) as usize,
                    data: &fragment.data,
                });
            }
        }

        overlays.sort_by_key(|slice| slice.ordinal);

        for slice in overlays {
            let end = slice.dest_offset + slice.length;
            let source_end = slice.source_offset + slice.length;
            buffer[slice.dest_offset..end]
                .copy_from_slice(&slice.data[slice.source_offset..source_end]);
        }
    }

    pub fn commit_locked<F>(
        &mut self,
        event_id: u64,
        disk_size_bytes: u64,
        mut write_range_fn: F,
    ) -> bool
    where
        F: FnMut(u64, &[u8]) -> Result<(), BackendError>,
    {
        let Some(record) = self.writes.get(&event_id).cloned() else {
            return true;
        };

        for fragment in record.fragments.values() {
            let end_offset = fragment
                .disk_offset_bytes
                .saturating_add(fragment.data.len() as u64);
            if end_offset < fragment.disk_offset_bytes || end_offset > disk_size_bytes {
                return false;
            }

            if !fragment.data.is_empty()
                && write_range_fn(fragment.disk_offset_bytes, &fragment.data).is_err()
            {
                return false;
            }
        }

        self.writes.remove(&event_id);
        true
    }

    pub fn reject_locked(&mut self, event_id: u64) {
        self.writes.remove(&event_id);
    }

    pub fn clear_locked(&mut self) {
        self.writes.clear();
        self.next_ordinal = 1;
    }
}
