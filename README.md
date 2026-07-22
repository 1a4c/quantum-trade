module dsl_hft_pipeline (
    input  wire        clk,           // 系統時脈 (例如 250MHz, 週期 4ns)
    input  wire        rst_n,         // 非同步低電位復位
    input  wire [31:0] in_price,      // 經過前段 Defused Filter 的價格 (Q16.16 定點數)
    input  wire        in_valid,      // 輸入資料有效訊號
    
    output reg  [1:0]  action_route,  // 00: Hold/Cancel, 01: Bid (Buy), 10: Ask (Sell)
    output reg         out_valid      // 輸出下單觸發訊號
);

    // --- Pipeline Registers (移位暫存器，用於二階微分) ---
    reg [31:0] r0_price, r1_price, r2_price;
    reg        v0, v1, v2;

    // --- Thresholds (設定套利門檻值) ---
    localparam signed [31:0] THRESHOLD_POS = 32'sd1000; // 正向加速度門檻
    localparam signed [31:0] THRESHOLD_NEG = -32'sd1000; // 負向加速度門檻

    // --- Stage 1: 資料推入暫存器鏈 ---
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            r0_price <= 32'd0;
            r1_price <= 32'd0;
            r2_price <= 32'd0;
            v0 <= 1'b0; v1 <= 1 me0; v2 <= 1'b0;
        end else if (in_valid) begin
            r0_price <= in_price;
            r1_price <= r0_price;
            r2_price <= r1_price;
            
            v0 <= in_valid;
            v1 <= v0;
            v2 <= v1;
        end
    end

    // --- Stage 2: 組合邏輯 - 計算二階微分 (Acceleration = R0 - 2*R1 + R2) ---
    wire signed [31:0] acceleration;
    assign acceleration = $signed(r0_price) - ($signed(r1_price) << 1) + $signed(r2_price);

    // --- Stage 3: 三態決策拆分與輸出 (bi_sub_tri & split_set) ---
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            action_route <= 2'b00;
            out_valid    <= 1'b0;
        end else if (v2) begin
            out_valid <= 1'b1;
            
            // 硬體並列比較器 (1 Clock Cycle 輸出)
            if (acceleration > THRESHOLD_POS) begin
                action_route <= 2'b01; // 觸發 BID Taker (買入)
            end else if (acceleration < THRESHOLD_NEG) begin
                action_route <= 2'b10; // 觸發 ASK Taker (賣出)
            end else begin
                action_route <= 2'b00; // Hold / 保持觀望
            end
        end else begin
            out_valid <= 1'b0;
        end
    end

endmodule
