<template>
    <BootstrapAlert :show="!hasValidData">
        <h4 class="alert-heading"><BIconInfoSquare class="fs-2" />&nbsp;{{ $t('gridprofile.NoInfo') }}</h4>
        {{ $t('gridprofile.NoInfoLong') }}
    </BootstrapAlert>

    <div class="d-flex justify-content-end mb-2" v-if="inverterSerial">
        <button type="button" class="btn btn-outline-secondary btn-sm" :disabled="refreshBusy" @click="refreshProfile">
            <span v-if="refreshBusy" class="spinner-border spinner-border-sm me-1" role="status"></span>
            {{ $t('gridprofile.Refresh') }}
        </button>
    </div>

    <template v-if="hasValidData">
        <BootstrapAlert :show="isModified" variant="warning">
            {{ $t('gridprofile.ModifiedWarning') }}
        </BootstrapAlert>

        <table class="table table-hover">
            <tbody>
                <tr>
                    <td>{{ $t('gridprofile.Name') }}</td>
                    <td>
                        {{ gridProfileList.name }}
                        <span v-if="isModified" class="badge bg-warning text-dark ms-1">
                            {{ $t('gridprofile.Modified') }}
                        </span>
                    </td>
                </tr>
                <tr>
                    <td>{{ $t('gridprofile.Version') }}</td>
                    <td>{{ gridProfileList.version }}</td>
                </tr>
            </tbody>
        </table>

        <!-- Write / edit control panel -->
        <div class="card mb-3" v-if="inverterSerial">
            <div class="card-header d-flex justify-content-between align-items-center">
                <span>{{ $t('gridprofile.WriteTitle') }}</span>
                <span v-if="writeStatus.running || writeStatus.state === 'Pending'" class="badge bg-warning text-dark">
                    {{ $t('gridprofile.WriteStatePending') }}
                </span>
                <span v-else-if="writeStatus.state === 'Ok'" class="badge bg-success">
                    {{ $t('gridprofile.WriteStateOk') }}
                </span>
                <span v-else-if="writeStatus.state === 'Failure'" class="badge bg-danger">
                    {{ $t('gridprofile.WriteStateFailure') }}
                </span>
            </div>
            <div class="card-body">
                <BootstrapAlert :show="writeAlertShow" :variant="writeAlertVariant">
                    {{ writeAlertMessage }}
                </BootstrapAlert>

                <BootstrapAlert :show="true" variant="warning">
                    <strong>{{ $t('gridprofile.WriteWarningTitle') }}</strong>
                    <div v-html="$t('gridprofile.WriteWarningBody')"></div>
                </BootstrapAlert>

                <div class="row g-2 align-items-end" v-if="presets.length > 0">
                    <div class="col-md-8">
                        <label class="form-label">{{ $t('gridprofile.WritePresetLabel') }}</label>
                        <select v-model="selectedPresetId" class="form-select" :disabled="writeBusy">
                            <option :value="-1" disabled>{{ $t('gridprofile.WritePresetChoose') }}</option>
                            <option v-for="p in presets" :key="p.id" :value="p.id">
                                {{ p.label }} ({{ p.size }} B)
                            </option>
                        </select>
                    </div>
                    <div class="col-md-4">
                        <button
                            type="button"
                            class="btn btn-primary w-100"
                            :disabled="writeBusy || selectedPresetId < 0"
                            @click="applyPreset"
                        >
                            {{ $t('gridprofile.WriteApplyPreset') }}
                        </button>
                    </div>
                </div>

                <div class="row g-2 mt-2">
                    <div class="col-md-8">
                        <div class="form-check form-switch">
                            <input
                                class="form-check-input"
                                type="checkbox"
                                id="editModeSwitch"
                                v-model="editMode"
                                :disabled="writeBusy"
                            />
                            <label class="form-check-label" for="editModeSwitch">
                                {{ $t('gridprofile.WriteEditModeToggle') }}
                            </label>
                        </div>
                    </div>
                    <div class="col-md-4">
                        <button
                            type="button"
                            class="btn btn-warning w-100"
                            :disabled="!editMode || writeBusy || !editedProfile"
                            @click="applyEdited"
                        >
                            {{ $t('gridprofile.WriteApplyEdits') }}
                        </button>
                    </div>
                </div>

                <div class="mt-2" v-if="writeStatus.running || writeStatus.queue_has_write">
                    <button type="button" class="btn btn-outline-danger btn-sm" @click="abortWrite">
                        {{ $t('gridprofile.WriteAbort') }}
                    </button>
                </div>
            </div>
        </div>

        <div class="accordion" id="accordionProfile">
            <div class="accordion-item accordion-table" v-for="(section, index) in displaySections" :key="index">
                <h2 class="accordion-header">
                    <button
                        class="accordion-button collapsed"
                        type="button"
                        data-bs-toggle="collapse"
                        :data-bs-target="`#collapse${index}`"
                        aria-expanded="true"
                        :aria-controls="`collapse${index}`"
                    >
                        {{ section.name }}
                    </button>
                </h2>
                <div :id="`collapse${index}`" class="accordion-collapse collapse" data-bs-parent="#accordionProfile">
                    <div class="accordion-body">
                        <table class="table table-hover">
                            <tbody>
                                <tr v-for="(value, vIdx) in section.items" :key="value.n + '_' + vIdx">
                                    <th>{{ value.n }}</th>
                                    <td>
                                        <template v-if="editMode && value.u != 'bool'">
                                            <div class="input-group input-group-sm">
                                                <input
                                                    type="number"
                                                    class="form-control"
                                                    step="any"
                                                    :value="value.v"
                                                    @input="
                                                        onEditValue(
                                                            index,
                                                            vIdx,
                                                            ($event.target as HTMLInputElement).value
                                                        )
                                                    "
                                                />
                                                <span class="input-group-text">{{ value.u }}</span>
                                            </div>
                                        </template>
                                        <template v-else-if="editMode && value.u == 'bool'">
                                            <select
                                                class="form-select form-select-sm"
                                                :value="value.v"
                                                @change="
                                                    onEditValue(index, vIdx, ($event.target as HTMLSelectElement).value)
                                                "
                                            >
                                                <option :value="1">{{ $t('gridprofile.Enabled') }}</option>
                                                <option :value="0">{{ $t('gridprofile.Disabled') }}</option>
                                            </select>
                                        </template>
                                        <template v-else-if="value.u != 'bool'">
                                            {{ $n(value.v, 'decimal') }} {{ value.u }}
                                        </template>
                                        <template v-else>
                                            <StatusBadge
                                                :status="value.v == 1"
                                                true_text="gridprofile.Enabled"
                                                false_text="gridprofile.Disabled"
                                            />
                                        </template>
                                    </td>
                                </tr>
                            </tbody>
                        </table>
                    </div>
                </div>
            </div>
        </div>

        <br />

        <div class="accordion" id="accordionDev">
            <div class="accordion-item">
                <h2 class="accordion-header">
                    <button
                        class="accordion-button collapsed"
                        type="button"
                        data-bs-toggle="collapse"
                        data-bs-target="#collapseDev"
                        aria-expanded="true"
                        aria-controls="collapseDev"
                    >
                        {{ $t('gridprofile.GridprofileSupport') }}
                    </button>
                </h2>
                <div id="collapseDev" class="accordion-collapse collapse" data-bs-parent="#accordionDev">
                    <div class="accordion-body">
                        <BootstrapAlert :show="true" variant="danger">
                            <h4 class="info-heading">
                                <BIconInfoSquare class="fs-2" />&nbsp;{{ $t('gridprofile.GridprofileSupport') }}
                            </h4>
                            <div v-html="$t('gridprofile.GridprofileSupportLong')"></div>
                        </BootstrapAlert>
                        <samp>
                            {{ rawContent() }}
                        </samp>
                    </div>
                </div>
            </div>
        </div>
    </template>
</template>

<script lang="ts">
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import type { GridProfileRawdata } from '@/types/GridProfileRawdata';
import type { GridProfileSection, GridProfileStatus } from '@/types/GridProfileStatus';
import type {
    GridProfileKnownList,
    GridProfilePreset,
    GridProfileWriteRequest,
    GridProfileWriteStatus,
} from '@/types/GridProfileWrite';
import { authHeader, handleResponse } from '@/utils/authentication';
import { BIconInfoSquare } from 'bootstrap-icons-vue';
import { defineComponent, type PropType } from 'vue';
import StatusBadge from './StatusBadge.vue';

interface EditedItem {
    n: string;
    u: string;
    v: number;
}
interface EditedSection {
    name: string;
    items: EditedItem[];
}

export default defineComponent({
    components: {
        BootstrapAlert,
        BIconInfoSquare,
        StatusBadge,
    },
    emits: ['refresh'],
    props: {
        gridProfileList: { type: Object as PropType<GridProfileStatus>, required: true },
        gridProfileRawList: { type: Object as PropType<GridProfileRawdata>, required: true },
        inverterSerial: { type: String, default: '' },
    },
    data() {
        return {
            presets: [] as GridProfilePreset[],
            selectedPresetId: -1 as number,
            editMode: false,
            editedSections: null as EditedSection[] | null,
            writeStatus: {
                running: false,
                state: 'Unknown',
                last_update: 0,
                queue_has_write: false,
            } as GridProfileWriteStatus,
            writeBusy: false,
            writeAlertShow: false,
            writeAlertVariant: 'info' as 'info' | 'success' | 'danger' | 'warning',
            writeAlertMessage: '',
            statusTimer: null as number | null,
            refreshBusy: false,
        };
    },
    computed: {
        rawContent() {
            return () => {
                return this.gridProfileRawList.raw
                    .map(function (x) {
                        let y = x.toString(16);
                        y = ('00' + y).substr(-2);
                        return y;
                    })
                    .join(' ');
            };
        },
        hasValidData(): boolean {
            return this.gridProfileRawList.raw.reduce((sum, x) => sum + x, 0) > 0;
        },
        displaySections(): GridProfileSection[] | EditedSection[] {
            if (this.editMode && this.editedSections) {
                return this.editedSections;
            }
            return this.gridProfileList.sections || [];
        },
        editedProfile(): boolean {
            return !!this.editedSections;
        },
        isModified(): boolean {
            return this.gridProfileList.matchedPresetId >= 0 && !this.gridProfileList.matchesPreset;
        },
    },
    watch: {
        editMode(newVal: boolean) {
            if (newVal) {
                this.editedSections = (this.gridProfileList.sections || []).map((s) => ({
                    name: s.name,
                    items: s.items.map((i) => ({ n: i.n, u: i.u, v: i.v })),
                }));
            } else {
                this.editedSections = null;
            }
        },
        inverterSerial: {
            immediate: true,
            handler(newVal: string) {
                if (newVal) {
                    this.fetchPresets();
                    this.fetchWriteStatus();
                    this.startPolling();
                }
            },
        },
    },
    beforeUnmount() {
        this.stopPolling();
    },
    methods: {
        onEditValue(sectionIdx: number, itemIdx: number, raw: string) {
            if (!this.editedSections) return;
            const section = this.editedSections[sectionIdx];
            if (!section) return;
            const item = section.items[itemIdx];
            if (!item) return;
            const n = Number(raw);
            if (Number.isFinite(n)) {
                item.v = n;
            }
        },
        fetchPresets() {
            fetch('/api/gridprofile/knownprofiles', { headers: authHeader() })
                .then((r) => (r.ok ? r.json() : Promise.reject(r.statusText)))
                .then((data: GridProfileKnownList) => {
                    this.presets = data.profiles || [];
                })
                .catch(() => {
                    this.presets = [];
                });
        },
        fetchWriteStatus() {
            if (!this.inverterSerial) return;
            fetch(`/api/gridprofile/writestatus?inv=${this.inverterSerial}`, { headers: authHeader() })
                .then((r) => (r.ok ? r.json() : Promise.reject(r.statusText)))
                .then((data: GridProfileWriteStatus) => {
                    this.writeStatus = data;
                })
                .catch(() => {
                    /* transient - keep last known state */
                });
        },
        startPolling() {
            this.stopPolling();
            this.statusTimer = window.setInterval(() => this.fetchWriteStatus(), 1500);
        },
        stopPolling() {
            if (this.statusTimer !== null) {
                window.clearInterval(this.statusTimer);
                this.statusTimer = null;
            }
        },
        postWrite(payload: GridProfileWriteRequest) {
            this.writeBusy = true;
            this.writeAlertShow = false;

            const formData = new FormData();
            formData.append('data', JSON.stringify(payload));

            fetch('/api/gridprofile/write', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then((response) => {
                    this.writeBusy = false;
                    this.writeAlertShow = true;
                    if (response && response.type === 'success') {
                        this.writeAlertVariant = 'success';
                        this.writeAlertMessage = this.$t('gridprofile.WriteQueued');
                        this.fetchWriteStatus();
                    } else {
                        this.writeAlertVariant = 'danger';
                        this.writeAlertMessage = (response && response.message) || 'Error';
                    }
                })
                .catch(() => {
                    this.writeBusy = false;
                    this.writeAlertShow = true;
                    this.writeAlertVariant = 'danger';
                    this.writeAlertMessage = 'Network error';
                });
        },
        applyPreset() {
            if (this.selectedPresetId < 0 || !this.inverterSerial) return;
            const ok = window.confirm(this.$t('gridprofile.WriteConfirm'));
            if (!ok) return;
            this.postWrite({
                serial: this.inverterSerial,
                preset_id: this.selectedPresetId,
            });
        },
        applyEdited() {
            if (!this.editedSections || !this.inverterSerial) return;
            const ok = window.confirm(this.$t('gridprofile.WriteConfirm'));
            if (!ok) return;
            this.postWrite({
                serial: this.inverterSerial,
                sections: this.editedSections.map((s) => ({
                    name: s.name,
                    items: s.items.map((i) => ({ n: i.n, u: i.u, v: i.v })),
                })),
            });
        },
        abortWrite() {
            if (!this.inverterSerial) return;
            const formData = new FormData();
            formData.append('data', JSON.stringify({ serial: this.inverterSerial }));
            fetch('/api/gridprofile/abort', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then(() => this.fetchWriteStatus());
        },
        refreshProfile() {
            if (!this.inverterSerial || this.refreshBusy) return;
            this.refreshBusy = true;
            const formData = new FormData();
            formData.append('data', JSON.stringify({ serial: this.inverterSerial }));
            fetch('/api/gridprofile/refresh', {
                method: 'POST',
                headers: authHeader(),
                body: formData,
            })
                .then((response) => handleResponse(response, this.$emitter, this.$router))
                .then(() => {
                    // Give the inverter time to answer the RF read before re-fetching.
                    window.setTimeout(() => {
                        this.refreshBusy = false;
                        this.$emit('refresh');
                    }, 1500);
                })
                .catch(() => {
                    this.refreshBusy = false;
                });
        },
    },
});
</script>
