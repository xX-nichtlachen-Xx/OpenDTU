<template>
    <BootstrapAlert :show="true" variant="warning">
        {{ $t('selfcheck.Hint') }}
    </BootstrapAlert>

    <BootstrapAlert :show="!selfCheckStatus.commands_enabled" variant="danger">
        {{ $t('selfcheck.CommandsDisabled') }}
    </BootstrapAlert>

    <table class="table table-hover">
        <tbody>
            <tr>
                <td>{{ $t('selfcheck.State') }}</td>
                <td>
                    <span class="badge" :class="stateBadgeClass">
                        {{ $t('selfcheck.State_' + selfCheckStatus.state) }}
                    </span>
                    <span v-if="isActive" class="spinner-border spinner-border-sm ms-2" role="status"></span>
                </td>
            </tr>
            <tr>
                <td>{{ $t('selfcheck.IpsState') }}</td>
                <td>{{ selfCheckStatus.ips_state }} ({{ selfCheckStatus.ips_state_name }})</td>
            </tr>
            <tr v-if="selfCheckStatus.profile_name">
                <td>{{ $t('selfcheck.Profile') }}</td>
                <td>
                    {{ selfCheckStatus.profile_name }} ({{ selfCheckStatus.profile_id }}) v{{
                        selfCheckStatus.profile_version
                    }}
                </td>
            </tr>
            <tr>
                <td>{{ $t('selfcheck.Elapsed') }}</td>
                <td>
                    {{ $t('selfcheck.ElapsedValue', { s: selfCheckStatus.elapsed, p: selfCheckStatus.polls }) }}
                </td>
            </tr>
            <tr v-if="selfCheckStatus.fragments.length > 0">
                <td>{{ $t('selfcheck.Fragments') }}</td>
                <td>
                    {{
                        $t('selfcheck.FragmentsValue', {
                            list: selfCheckStatus.fragments.join(', '),
                            total: selfCheckStatus.fragments_total > 0 ? selfCheckStatus.fragments_total : '?',
                        })
                    }}
                </td>
            </tr>
        </tbody>
    </table>

    <div class="border rounded p-2 overflow-auto selfcheck-box">
        <template v-if="selfCheckStatus.items.length > 0">
            <div class="table-responsive">
                <table class="table table-sm table-hover">
                    <thead>
                        <tr>
                            <th>{{ $t('selfcheck.Test') }}</th>
                            <th>{{ $t('selfcheck.TripValue') }}</th>
                            <th>{{ $t('selfcheck.TripTime') }}</th>
                            <th>{{ $t('selfcheck.MeasValue') }}</th>
                            <th>{{ $t('selfcheck.MeasTime') }}</th>
                            <th>{{ $t('selfcheck.Status') }}</th>
                        </tr>
                    </thead>
                    <tbody>
                        <tr v-for="row in reportRows" :key="row.id">
                            <td>{{ $t('selfcheck.Item_' + row.id) }}</td>
                            <template v-if="row.item">
                                <td>{{ formatValue(row.item) }}</td>
                                <td>{{ formatTime(row.item, row.item.set_time) }}</td>
                                <td>{{ formatMeasValue(row.item) }}</td>
                                <td>{{ formatTime(row.item, row.item.meas_time) }}</td>
                                <td>
                                    <span v-if="row.item.passed" class="badge text-bg-success">{{
                                        $t('selfcheck.Success')
                                    }}</span>
                                    <span v-else class="badge text-bg-danger">{{ $t('selfcheck.Failed') }}</span>
                                </td>
                            </template>
                            <template v-else>
                                <td colspan="4">-</td>
                                <td>
                                    <span class="badge text-bg-danger">{{ $t('selfcheck.Failed') }}</span>
                                </td>
                            </template>
                        </tr>
                    </tbody>
                </table>
            </div>
        </template>
        <div v-else class="text-muted">
            {{ isActive ? $t('selfcheck.Waiting') : $t('selfcheck.NoResult') }}
        </div>
    </div>

    <div class="accordion mt-3" id="accordionSelfCheckRaw" v-if="selfCheckStatus.raw.length > 0">
        <div class="accordion-item">
            <h2 class="accordion-header">
                <button
                    class="accordion-button collapsed"
                    type="button"
                    data-bs-toggle="collapse"
                    data-bs-target="#collapseSelfCheckRaw"
                    aria-expanded="false"
                    aria-controls="collapseSelfCheckRaw"
                >
                    {{ $t('selfcheck.RawData') }}
                </button>
            </h2>
            <div id="collapseSelfCheckRaw" class="accordion-collapse collapse" data-bs-parent="#accordionSelfCheckRaw">
                <div class="accordion-body">
                    <samp class="d-block text-break">{{ rawContent }}</samp>
                </div>
            </div>
        </div>
    </div>
</template>

<script lang="ts">
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import type { SelfCheckItem, SelfCheckStatus } from '@/types/SelfCheckStatus';
import { defineComponent, type PropType } from 'vue';

const REPORT_ORDER = ['ov1', 'ov2', 'uv1', 'uv2', 'of1', 'of2', 'uf1', 'uf2'];

export default defineComponent({
    components: {
        BootstrapAlert,
    },
    props: {
        selfCheckStatus: { type: Object as PropType<SelfCheckStatus>, required: true },
    },
    computed: {
        reportRows(): Array<{ id: string; item: SelfCheckItem | undefined }> {
            return REPORT_ORDER.map((id) => ({ id: id, item: this.getItem(id) }));
        },
        isActive(): boolean {
            return (
                this.selfCheckStatus.state == 'starting' ||
                this.selfCheckStatus.state == 'running' ||
                this.selfCheckStatus.state == 'collecting'
            );
        },
        stateBadgeClass(): string {
            switch (this.selfCheckStatus.state) {
                case 'starting':
                case 'running':
                case 'collecting':
                    return 'text-bg-warning';
                case 'finished':
                    return this.selfCheckStatus.ips_state == 2 ? 'text-bg-success' : 'text-bg-danger';
                case 'timeout':
                case 'reset':
                case 'start_failed':
                case 'unsupported':
                case 'incomplete':
                    return 'text-bg-danger';
                default:
                    return 'text-bg-secondary';
            }
        },
        rawContent(): string {
            return this.selfCheckStatus.raw
                .map((x) => {
                    return x.toString(16).toUpperCase().padStart(2, '0');
                })
                .join(' ');
        },
    },
    methods: {
        getItem(id: string): SelfCheckItem | undefined {
            return this.selfCheckStatus.items.find((item) => item.id == id);
        },
        formatNumber(value: number, digits: number): string {
            return Number(value.toFixed(digits)).toString();
        },
        formatValue(item: SelfCheckItem): string {
            return this.formatNumber(item.set_value, item.value_digits) + item.unit;
        },
        formatMeasValue(item: SelfCheckItem): string {
            return this.formatNumber(item.meas_value, item.value_digits) + item.unit;
        },
        formatTime(item: SelfCheckItem, time: number): string {
            return this.formatNumber(time, item.time_digits) + 's';
        },
    },
});
</script>

<style scoped>
.selfcheck-box {
    max-height: 50vh;
}
</style>
