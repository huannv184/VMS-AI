// ============================================================================
// common/lockfree_queue.h
// Thread-safety: Thread-safe (lock-free). Safe for single-producer 
// single-consumer (SPSC) usage. Raw new/delete is intentional here
// because std::unique_ptr cannot be used with std::atomic operations
// required for lock-free algorithms.
// ============================================================================
#pragma once
#include <atomic>
#include <memory>
#include <optional>

namespace edge {

template<typename T>
class LockFreeQueue {
private:
    struct Node {
        std::shared_ptr<T> data;
        std::atomic<Node*> next;
        Node() : next(nullptr) {}
    };
    
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    
public:
    LockFreeQueue() {
        Node* dummy = new Node();
        head_.store(dummy);
        tail_.store(dummy);
    }
    
    ~LockFreeQueue() {
        while(Node* old_head = head_.load()) {
            head_.store(old_head->next);
            delete old_head;
        }
    }
    
    void push(T value) {
        auto data = std::make_shared<T>(std::move(value));
        Node* new_node = new Node();
        Node* old_tail = tail_.load();
        old_tail->data = data;
        old_tail->next.store(new_node);
        tail_.store(new_node);
    }
    
    std::optional<T> pop() {
        Node* old_head = head_.load();
        if(old_head == tail_.load()) {
            return std::nullopt;
        }
        auto result = old_head->data;
        head_.store(old_head->next);
        delete old_head;
        return *result;
    }
    
    bool empty() const {
        return head_.load() == tail_.load();
    }
};

} // namespace edge

