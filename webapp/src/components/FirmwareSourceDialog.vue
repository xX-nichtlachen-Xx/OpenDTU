<template>
    <ModalDialog modalId="firmwareSourceView" :title="$t('firmwaresource.Title')">
        <p class="text-muted small mb-3">
            {{ $t('firmwaresource.ForInverter', { model: modelName || '-', serial: serial }) }}
        </p>

        <BootstrapAlert v-model="showAlert" :variant="alertType" :jumpToTop="false" dismissible class="mb-3">
            {{ alertMessage }}
        </BootstrapAlert>

        <div class="btn-group w-100 mb-3" role="group">
            <input
                type="radio"
                class="btn-check"
                id="fwSourceExisting"
                value="existing"
                v-model="mode"
                :disabled="busy"
                autocomplete="off"
            />
            <label class="btn btn-outline-primary" for="fwSourceExisting">
                <BIconHdd class="me-1" />{{ $t('firmwaresource.OptionExisting') }}
            </label>

            <input
                type="radio"
                class="btn-check"
                id="fwSourceOnline"
                value="online"
                v-model="mode"
                :disabled="busy"
                autocomplete="off"
            />
            <label class="btn btn-outline-primary" for="fwSourceOnline">
                <BIconCloudDownload class="me-1" />{{ $t('firmwaresource.OptionOnline') }}
            </label>

            <input
                type="radio"
                class="btn-check"
                id="fwSourceLocal"
                value="local"
                v-model="mode"
                :disabled="busy"
                autocomplete="off"
            />
            <label class="btn btn-outline-primary" for="fwSourceLocal">
                <BIconUpload class="me-1" />{{ $t('firmwaresource.OptionLocal') }}
            </label>
        </div>

        <!-- Option 1: file already buffered on the DTU -->
        <div v-if="mode === 'existing'">
            <div v-if="bufferLoading" class="text-center">
                <div class="spinner-border" role="status">
                    <span class="visually-hidden">{{ $t('base.Loading') }}</span>
                </div>
            </div>
            <table v-else-if="bufferInfo.size > 0" class="table table-sm mb-0">
                <tbody>
                    <tr>
                        <td>{{ $t('firmwaresource.BufferName') }}</td>
                        <td>{{ bufferInfo.name || $t('firmwaresource.UnknownName') }}</td>
                    </tr>
                    <tr>
                        <td>{{ $t('firmwaresource.BufferSize') }}</td>
                        <td>{{ formatSize(bufferInfo.size) }}</td>
                    </tr>
                    <tr>
                        <td>{{ $t('firmwaresource.BufferSource') }}</td>
                        <td>
                            {{
                                bufferInfo.source === 'psram'
                                    ? $t('firmwaresource.SourcePsram')
                                    : $t('firmwaresource.SourceOta')
                            }}
                        </td>
                    </tr>
                    <tr v-if="lastHexInfo">
                        <td>{{ $t('firmwaresource.BufferVersion') }}</td>
                        <td>{{ describeHex(lastHexInfo) }}</td>
                    </tr>
                </tbody>
            </table>
            <div v-else class="alert alert-warning mb-0" role="alert">
                {{ $t('firmwaresource.NoFileOnDtu') }}
            </div>
            <div class="alert alert-info mt-3 mb-0" role="alert">
                {{ $t('firmwaresource.CheckedOnStartHint') }}
            </div>
        </div>

        <!-- Option 2: pick from the GitHub manifest -->
        <div v-else-if="mode === 'online'">
            <div v-if="manifestLoading" class="text-center">
                <div class="spinner-border" role="status">
                    <span class="visually-hidden">{{ $t('base.Loading') }}</span>
                </div>
            </div>
            <div v-else-if="manifestError" class="alert alert-danger mb-0" role="alert">
                {{ $t('firmwaresource.ManifestError') }}
                <div class="small text-muted mt-1">{{ manifestError }}</div>
                <button type="button" class="btn btn-sm btn-outline-danger mt-2" @click="loadManifest">
                    {{ $t('base.Reload') }}
                </button>
            </div>
            <template v-else-if="manifest">
                <div class="d-flex flex-wrap align-items-center gap-3 mb-2">
                    <div class="form-check form-switch mb-0">
                        <input
                            class="form-check-input"
                            type="checkbox"
                            role="switch"
                            id="fwSourceOnlyMatching"
                            v-model="onlyMatching"
                        />
                        <label class="form-check-label" for="fwSourceOnlyMatching">
                            {{ $t('firmwaresource.OnlyMatching', { prefix: serialPrefix }) }}
                        </label>
                    </div>
                    <select v-if="!onlyMatching" class="form-select form-select-sm w-auto" v-model="familyFilter">
                        <option value="">{{ $t('firmwaresource.AllFamilies') }}</option>
                        <option v-for="family in families" :key="family" :value="family">{{ family }}</option>
                    </select>
                    <span class="ms-auto small text-muted">
                        {{ $t('firmwaresource.ManifestGenerated', { date: formatDate(manifest.generated) }) }}
                        <span v-if="manifest.changelog_url">
                            &middot;
                            <a :href="manifest.changelog_url" target="_blank" rel="noopener">
                                {{ $t('firmwaresource.Changelog') }}
                            </a>
                        </span>
                    </span>
                </div>

                <div v-if="filteredFiles.length === 0" class="alert alert-info mb-0" role="alert">
                    {{ onlyMatching ? $t('firmwaresource.NoMatching') : $t('firmwaresource.NoFiles') }}
                </div>
                <div v-else class="table-responsive border rounded" style="max-height: 40vh; overflow-y: auto">
                    <table class="table table-sm table-hover mb-0">
                        <thead class="table-light sticky-top">
                            <tr>
                                <th>{{ $t('firmwaresource.ColName') }}</th>
                                <th>{{ $t('firmwaresource.ColVersion') }}</th>
                                <th>{{ $t('firmwaresource.ColSize') }}</th>
                                <th>{{ $t('firmwaresource.ColDate') }}</th>
                            </tr>
                        </thead>
                        <tbody>
                            <tr
                                v-for="file in filteredFiles"
                                :key="file.path"
                                role="button"
                                :class="{ 'table-primary': selectedFile !== null && selectedFile.path === file.path }"
                                @click="selectedFile = file"
                            >
                                <td>
                                    {{ file.name }}
                                    <div class="small text-muted">{{ file.folder || file.family }}</div>
                                </td>
                                <td>{{ file.version }}</td>
                                <td>{{ formatSize(file.size) }}</td>
                                <td>{{ formatDate(file.date) }}</td>
                            </tr>
                        </tbody>
                    </table>
                </div>

                <div class="d-flex flex-wrap align-items-center gap-2 mt-3">
                    <button
                        type="button"
                        class="btn btn-primary"
                        :disabled="selectedFile === null || busy"
                        @click="onDownloadSelected"
                    >
                        <BIconCloudDownload class="me-1" />{{ $t('firmwaresource.DownloadAndStore') }}
                    </button>
                    <span v-if="selectedFile !== null" class="small text-muted">
                        {{ selectedFile.name }} &middot; {{ selectedFile.version }} &middot;
                        {{ $t('firmwaresource.Rows', { rows: selectedFile.rows }) }}
                    </span>
                </div>
            </template>
        </div>

        <!-- Option 3: local .hex file -->
        <div v-else>
            <input
                class="form-control"
                type="file"
                accept=".hex"
                ref="localFileInput"
                :disabled="busy"
                @change="onLocalFileChange"
            />
            <div v-if="localHexInfo" class="small text-muted mt-2">
                {{ localFileName }} &middot; {{ formatSize(localBytes ? localBytes.byteLength : 0) }} &middot;
                {{ describeHex(localHexInfo) }}
            </div>
            <button
                type="button"
                class="btn btn-primary mt-3"
                :disabled="localBytes === null || busy"
                @click="onUploadLocal"
            >
                <BIconUpload class="me-1" />{{ $t('firmwaresource.StoreOnDtu') }}
            </button>
        </div>

        <div v-if="busy" class="mt-3">
            <div class="small mb-1">{{ phaseText }}</div>
            <div class="progress" style="height: 12px">
                <div
                    class="progress-bar progress-bar-striped progress-bar-animated"
                    role="progressbar"
                    :style="{ width: `${progress}%` }"
                    :aria-valuenow="progress"
                    aria-valuemin="0"
                    aria-valuemax="100"
                ></div>
            </div>
        </div>

        <template #footer>
            <button
                type="button"
                class="btn btn-danger me-2"
                :disabled="busy || bufferLoading || bufferInfo.size === 0"
                @click="$emit('flash')"
            >
                <BIconLightningCharge class="me-1" />{{ $t('firmwaresource.FlashNow') }}
            </button>
        </template>
    </ModalDialog>
</template>

<script lang="ts">
import BootstrapAlert from '@/components/BootstrapAlert.vue';
import ModalDialog from '@/components/ModalDialog.vue';
import type { FirmwareBufferInfo, FirmwareManifest, FirmwareManifestFile } from '@/types/FirmwareManifest';
import { authHeader, handleResponse } from '@/utils/authentication';
import {
    downloadFirmwareFile,
    fetchFirmwareManifest,
    firmwareDownloadUrl,
    firmwareSerialPrefix,
    FirmwareUploadError,
    formatByteSize,
    uploadInverterFirmware,
} from '@/utils/firmwareSource';
import { intelHexErrorMessage, MAX_FIRMWARE_UPLOAD_SIZE, validateIntelHex, type IntelHexInfo } from '@/utils/intelHex';
import { BIconCloudDownload, BIconHdd, BIconLightningCharge, BIconUpload } from 'bootstrap-icons-vue';
import { defineComponent } from 'vue';

type SourceMode = 'existing' | 'online' | 'local';
type Phase = 'idle' | 'downloading' | 'validating' | 'uploading';

export default defineComponent({
    components: {
        BootstrapAlert,
        ModalDialog,
        BIconCloudDownload,
        BIconHdd,
        BIconLightningCharge,
        BIconUpload,
    },
    props: {
        serial: { type: String, required: true },
        modelName: { type: String, default: '' },
    },
    emits: ['flash'],
    data() {
        return {
            mode: 'existing' as SourceMode,
            bufferInfo: { name: '', variant: '', source: 'none', size: 0 } as FirmwareBufferInfo,
            bufferLoading: false,
            lastHexInfo: null as IntelHexInfo | null,

            manifest: null as FirmwareManifest | null,
            manifestLoading: false,
            manifestError: '',
            onlyMatching: true,
            familyFilter: '',
            selectedFile: null as FirmwareManifestFile | null,

            localBytes: null as ArrayBuffer | null,
            localFileName: '',
            localHexInfo: null as IntelHexInfo | null,

            phase: 'idle' as Phase,
            progress: 0,
            showAlert: false,
            alertType: 'info',
            alertMessage: '',
        };
    },
    computed: {
        busy(): boolean {
            return this.phase !== 'idle';
        },
        serialPrefix(): string {
            return firmwareSerialPrefix(this.serial);
        },
        families(): string[] {
            if (!this.manifest) {
                return [];
            }
            return Array.from(new Set(this.manifest.files.map((f) => f.family))).sort();
        },
        filteredFiles(): FirmwareManifestFile[] {
            if (!this.manifest) {
                return [];
            }
            let files = this.manifest.files;
            if (this.onlyMatching) {
                files = files.filter((f) => f.serial_prefixes.includes(this.serialPrefix));
            } else if (this.familyFilter) {
                files = files.filter((f) => f.family === this.familyFilter);
            }
            return files.slice().sort((a, b) => {
                const family = a.family.localeCompare(b.family);
                if (family !== 0) return family;
                const name = a.name.localeCompare(b.name, undefined, { numeric: true });
                if (name !== 0) return name;
                return b.version_code - a.version_code;
            });
        },
        phaseText(): string {
            switch (this.phase) {
                case 'downloading':
                    return this.$t('firmwaresource.Downloading', { pct: this.progress });
                case 'validating':
                    return this.$t('firmwaresource.Validating');
                case 'uploading':
                    return this.$t('firmwaresource.Uploading', { pct: this.progress });
                default:
                    return '';
            }
        },
    },
    watch: {
        mode(newMode: SourceMode) {
            if (newMode === 'online' && this.manifest === null && !this.manifestLoading) {
                this.loadManifest();
            }
        },
    },
    methods: {
        // Called by the parent right before the modal is shown for a
        // (possibly different) inverter.
        reset() {
            this.phase = 'idle';
            this.progress = 0;
            this.showAlert = false;
            this.selectedFile = null;
            this.localBytes = null;
            this.localFileName = '';
            this.localHexInfo = null;
            this.onlyMatching = true;
            this.familyFilter = '';
            const input = this.$refs.localFileInput as HTMLInputElement | undefined;
            if (input) {
                input.value = '';
            }
            this.loadBufferInfo().then(() => {
                // Default to the buffered image when there is one, otherwise
                // jump straight to the online picker.
                this.mode = this.bufferInfo.size > 0 ? 'existing' : 'online';
            });
        },
        formatSize(bytes: number): string {
            return formatByteSize(bytes);
        },
        formatDate(iso: string): string {
            const d = new Date(iso);
            return isNaN(d.getTime()) ? iso : d.toLocaleDateString();
        },
        describeHex(info: IntelHexInfo): string {
            const version = info.version || this.$t('firmwaresource.UnknownVersion');
            return this.$t('firmwaresource.HexInfo', { version, rows: info.rows });
        },
        setAlert(type: string, message: string) {
            this.alertType = type;
            this.alertMessage = message;
            this.showAlert = true;
        },
        async loadBufferInfo(): Promise<void> {
            this.bufferLoading = true;
            try {
                const response = await fetch('/api/file/firmware_info', { headers: authHeader() });
                const data = (await handleResponse(response, this.$emitter, this.$router)) as FirmwareBufferInfo;
                this.bufferInfo = {
                    name: data.name || '',
                    variant: data.variant || '',
                    source: data.source || 'none',
                    size: data.size || 0,
                };
            } catch {
                this.bufferInfo = { name: '', variant: '', source: 'none', size: 0 };
            } finally {
                this.bufferLoading = false;
            }
        },
        async loadManifest(): Promise<void> {
            this.manifestLoading = true;
            this.manifestError = '';
            try {
                this.manifest = await fetchFirmwareManifest();
            } catch (e) {
                this.manifest = null;
                this.manifestError = e instanceof Error ? e.message : String(e);
            } finally {
                this.manifestLoading = false;
            }
        },
        async onDownloadSelected(): Promise<void> {
            const file = this.selectedFile;
            if (!file || !this.manifest) {
                return;
            }
            this.showAlert = false;
            this.phase = 'downloading';
            this.progress = 0;
            try {
                const bytes = await downloadFirmwareFile(firmwareDownloadUrl(this.manifest, file.path), (pct) => {
                    this.progress = pct;
                });
                if (bytes.byteLength !== file.size) {
                    throw new Error(
                        this.$t('firmwaresource.SizeMismatch', {
                            expected: this.formatSize(file.size),
                            actual: this.formatSize(bytes.byteLength),
                        })
                    );
                }
                await this.validateAndStore(bytes, file.name + '.hex');
            } catch (e) {
                this.phase = 'idle';
                this.setAlert('danger', this.describeError(e, this.$t('firmwaresource.DownloadError')));
            }
        },
        async onLocalFileChange(): Promise<void> {
            const input = this.$refs.localFileInput as HTMLInputElement;
            const file = input.files && input.files[0] ? input.files[0] : null;
            this.localBytes = null;
            this.localHexInfo = null;
            this.localFileName = '';
            this.showAlert = false;
            if (!file) {
                return;
            }
            const bytes = await file.arrayBuffer();
            const check = this.checkImage(bytes);
            if (typeof check === 'string') {
                this.setAlert('danger', check);
                return;
            }
            this.localBytes = bytes;
            this.localFileName = file.name;
            this.localHexInfo = check;
        },
        async onUploadLocal(): Promise<void> {
            if (!this.localBytes) {
                return;
            }
            this.showAlert = false;
            try {
                await this.validateAndStore(this.localBytes, this.localFileName);
            } catch (e) {
                this.phase = 'idle';
                this.setAlert('danger', this.describeError(e, this.$t('firmwaresource.UploadError')));
            }
        },
        // Validates the image in the browser and returns its info, or a ready
        // to display error message.
        checkImage(bytes: ArrayBuffer): IntelHexInfo | string {
            if (bytes.byteLength > MAX_FIRMWARE_UPLOAD_SIZE) {
                return this.$t('firmwaresource.TooLarge', {
                    size: this.formatSize(bytes.byteLength),
                    max: this.formatSize(MAX_FIRMWARE_UPLOAD_SIZE),
                });
            }
            const validation = validateIntelHex(new TextDecoder().decode(bytes));
            if (!validation.ok) {
                const message = intelHexErrorMessage(validation);
                return this.$t('firmwaresource.' + message.key, message.params);
            }
            return validation.info;
        },
        async validateAndStore(bytes: ArrayBuffer, origName: string): Promise<void> {
            this.phase = 'validating';
            this.progress = 0;
            const check = this.checkImage(bytes);
            if (typeof check === 'string') {
                throw new Error(check);
            }

            this.phase = 'uploading';
            await uploadInverterFirmware(new Blob([bytes]), origName, (pct) => {
                this.progress = pct;
            });

            await this.loadBufferInfo();
            this.phase = 'idle';
            this.lastHexInfo = check;
            this.mode = 'existing';
            if (this.bufferInfo.size !== bytes.byteLength) {
                this.setAlert(
                    'warning',
                    this.$t('firmwaresource.DtuSizeMismatch', {
                        sent: this.formatSize(bytes.byteLength),
                        stored: this.formatSize(this.bufferInfo.size),
                    })
                );
                return;
            }
            this.setAlert('success', this.$t('firmwaresource.StoreSuccess', { name: origName }));
        },
        describeError(e: unknown, fallback: string): string {
            if (e instanceof FirmwareUploadError) {
                return e.message || this.$t('firmwaresource.UploadError');
            }
            if (e instanceof Error && e.message) {
                return e.message === 'network' || e.message === 'aborted' || e.message.startsWith('HTTP ')
                    ? `${fallback} (${e.message})`
                    : e.message;
            }
            return fallback;
        },
    },
});
</script>
