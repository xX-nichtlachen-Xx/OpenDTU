export interface SelfCheckItem {
    id: string;
    unit: string;
    code: string;
    passed: boolean;
    value_digits: number;
    time_digits: number;
    set_value: number;
    set_time: number;
    meas_value: number;
    meas_time: number;
}

export interface SelfCheckStatus {
    state: string;
    commands_enabled: boolean;
    elapsed: number;
    polls: number;
    ips_state: number;
    ips_state_name: string;
    profile_id: string;
    profile_name: string;
    profile_version: string;
    items: Array<SelfCheckItem>;
    fragments: Array<number>;
    fragments_total: number;
    raw: Array<number>;
}
