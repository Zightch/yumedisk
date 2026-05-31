#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct ListNodeId(usize);

impl ListNodeId {
    fn index(self) -> usize {
        self.0
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ListNode {
    value: u64,
    prev: Option<ListNodeId>,
    next: Option<ListNodeId>,
}

#[derive(Debug, Default)]
pub(crate) struct IndexedList {
    nodes: Vec<Option<ListNode>>,
    free: Vec<usize>,
    head: Option<ListNodeId>,
    tail: Option<ListNodeId>,
    len: usize,
}

impl IndexedList {
    pub(crate) fn len(&self) -> usize {
        self.len
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub(crate) fn front_value(&self) -> Option<u64> {
        self.head
            .and_then(|node| self.node(node).map(|entry| entry.value))
    }

    pub(crate) fn push_back(&mut self, value: u64) -> ListNodeId {
        let node_id = self.alloc_node(value);
        let old_tail = self.tail;

        if let Some(tail) = old_tail {
            self.node_mut(tail).unwrap().next = Some(node_id);
        } else {
            self.head = Some(node_id);
        }

        let node = self.node_mut(node_id).unwrap();
        node.prev = old_tail;
        node.next = None;
        self.tail = Some(node_id);
        self.len += 1;
        node_id
    }

    pub(crate) fn pop_front(&mut self) -> Option<u64> {
        let head = self.head?;
        self.remove(head)
    }

    pub(crate) fn remove(&mut self, node_id: ListNodeId) -> Option<u64> {
        let node = self.nodes.get_mut(node_id.index())?.take()?;

        if let Some(prev) = node.prev {
            self.node_mut(prev).unwrap().next = node.next;
        } else {
            self.head = node.next;
        }

        if let Some(next) = node.next {
            self.node_mut(next).unwrap().prev = node.prev;
        } else {
            self.tail = node.prev;
        }

        self.free.push(node_id.index());
        self.len -= 1;
        Some(node.value)
    }

    pub(crate) fn move_to_back(&mut self, node_id: ListNodeId) -> bool {
        if self.tail == Some(node_id) {
            return self.node(node_id).is_some();
        }

        let (prev, next) = match self.node(node_id) {
            Some(node) => (node.prev, node.next),
            None => return false,
        };

        if let Some(prev) = prev {
            self.node_mut(prev).unwrap().next = next;
        } else {
            self.head = next;
        }

        if let Some(next) = next {
            self.node_mut(next).unwrap().prev = prev;
        }

        let old_tail = self.tail;
        let node = self.node_mut(node_id).unwrap();
        node.prev = old_tail;
        node.next = None;

        if let Some(tail) = old_tail {
            self.node_mut(tail).unwrap().next = Some(node_id);
        } else {
            self.head = Some(node_id);
        }

        self.tail = Some(node_id);
        true
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub(crate) fn values_from_head(&self) -> Vec<u64> {
        let mut values = Vec::with_capacity(self.len);
        let mut cursor = self.head;
        while let Some(node) = cursor {
            let entry = self.node(node).unwrap();
            values.push(entry.value);
            cursor = entry.next;
        }
        values
    }

    fn alloc_node(&mut self, value: u64) -> ListNodeId {
        let node = ListNode {
            value,
            prev: None,
            next: None,
        };

        if let Some(index) = self.free.pop() {
            self.nodes[index] = Some(node);
            return ListNodeId(index);
        }

        let index = self.nodes.len();
        self.nodes.push(Some(node));
        ListNodeId(index)
    }

    fn node(&self, node_id: ListNodeId) -> Option<&ListNode> {
        self.nodes.get(node_id.index())?.as_ref()
    }

    fn node_mut(&mut self, node_id: ListNodeId) -> Option<&mut ListNode> {
        self.nodes.get_mut(node_id.index())?.as_mut()
    }
}

#[cfg(test)]
mod tests {
    use super::IndexedList;

    #[test]
    fn push_back_and_pop_front_preserve_fifo_order() {
        let mut list = IndexedList::default();
        list.push_back(1);
        list.push_back(2);
        list.push_back(3);

        assert_eq!(list.values_from_head(), vec![1, 2, 3]);
        assert_eq!(list.pop_front(), Some(1));
        assert_eq!(list.pop_front(), Some(2));
        assert_eq!(list.pop_front(), Some(3));
        assert_eq!(list.pop_front(), None);
        assert!(list.is_empty());
    }

    #[test]
    fn remove_detaches_middle_node() {
        let mut list = IndexedList::default();
        let _first = list.push_back(1);
        let middle = list.push_back(2);
        let _last = list.push_back(3);

        assert_eq!(list.remove(middle), Some(2));
        assert_eq!(list.values_from_head(), vec![1, 3]);
        assert_eq!(list.len(), 2);
    }

    #[test]
    fn move_to_back_relinks_existing_node() {
        let mut list = IndexedList::default();
        let first = list.push_back(1);
        list.push_back(2);
        list.push_back(3);

        assert!(list.move_to_back(first));
        assert_eq!(list.values_from_head(), vec![2, 3, 1]);
        assert_eq!(list.front_value(), Some(2));
    }
}
