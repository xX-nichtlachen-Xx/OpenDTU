export interface YieldTotalStatus {
    yield_total_set_status: string;
}

export interface YieldTotalConfig {
    serial: string;
    // Per-string total produced energy in Wh. Length must be 1, 2 or 4.
    values: number[];
}
