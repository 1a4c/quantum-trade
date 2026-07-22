#include <iostream>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

// 模擬 Cache Line 對齊 (64 Bytes) 以避免 False Sharing
struct alignas(64) OrderBookTick {
    uint64_t timestamp_ns;
    double bid_price;
    double ask_price;
    uint32_t bid_volume;
    uint32_t ask_volume;
};

// 1. 無鎖環形佇列 (GO4_queue_ring / Lock-free Ring Buffer)
template <typename T, size_t Size>
class LockFreeRingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2"); // 2的冪次方便快速位元運算
private:
    std::array<T, Size> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & (Size - 1);
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; // Queue full
        }
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue empty
        }
        item = buffer_[current_head];
        head_.store((current_head + 1) & (Size - 1), std::memory_order_release);
        return true;
    }
};

// 2. 核心訊號引擎：動量二階微分與 VWAP 奇數項濾波 (rotate_conjugate_double_differentiated)
class HFTSignalEngine {
public:
    // 價格動量二階微分 (Calculate Price Acceleration)
    static inline double calculate_price_acceleration(double p_prev, double p_curr, double p_next) {
        // 二階有限差分 (d²P / dt²) 評估買賣盤吞吐加速度
        return p_next - (2.0 * p_curr) + p_prev;
    }

    // 奇數/噪點訊號去化 (VWAP Odd-signal Defuser)
    static inline double defuse_odd_noise(double raw_spread, double prev_spread) {
        // 對高頻震盪的微幅 Spread 進行平滑過濾
        return (raw_spread * 0.7) + (prev_spread * 0.3);
    }
};

// 3. 決策拆分 (bi_sub_tri & split_set)
enum class ExecutionAction { BID_TAKER, ASK_TAKER, CANCEL_WAIT };

ExecutionAction split_set_bi_sub_tri(double acceleration, double filtered_spread) {
    // 依據二階加速度與過濾後的價差判斷交易路徑
    constexpr double THRESHOLD = 0.05;
    
    if (acceleration > THRESHOLD && filtered_spread > 0.01) {
        return ExecutionAction::BID_TAKER; // 強烈向上動力，主動吃單買入
    } else if (acceleration < -THRESHOLD && filtered_spread > 0.01) {
        return ExecutionAction::ASK_TAKER; // 強烈向下動力，主動吃單賣出
    }
    return ExecutionAction::CANCEL_WAIT;   // 處於平靜期 (Relaxed Period)，維持觀望/撤單
}

int main() {
    // 初始化 1024 大小的極速 Ring Buffer
    LockFreeRingBuffer<OrderBookTick, 1024> market_data_queue;

    // 模擬傳入微秒級行情
    market_data_queue.push({1700000000000, 150.00, 150.05, 100, 120});
    market_data_queue.push({1700000000001, 150.02, 150.06, 110, 80});
    market_data_queue.push({1700000000002, 150.08, 150.10, 300, 20}); // 急漲行情

    OrderBookTick t0, t1, t2;
    if (market_data_queue.pop(t0) && market_data_queue.pop(t1) && market_data_queue.pop(t2)) {
        
        // Step 1: 價差去噪 (Defuse)
        double spread0 = t0.ask_price - t0.bid_price;
        double spread1 = t1.ask_price - t1.bid_price;
        double clean_spread = HFTSignalEngine::defuse_odd_noise(spread1, spread0);

        // Step 2: 二階加速度計算 (Double Differentiated Momentum)
        double accel = HFTSignalEngine::calculate_price_acceleration(t0.bid_price, t1.bid_price, t2.bid_price);

        // Step 3: 三向決策拆分 (Bi-sub-tri Split)
        ExecutionAction action = split_set_bi_sub_tri(accel, clean_spread);

        // 輸出觸發的 HFT 指令
        std::cout << "[HFT Engine Pipeline Output]\n";
        std::cout << "Clean Spread: " << clean_spread << " | Price Accel: " << accel << "\n";
        std::cout << "Action Route: " << (action == ExecutionAction::BID_TAKER ? "SEND BUY ORDER (IOC)" : "HOLD") << "\n";
    }

    return 0;
}