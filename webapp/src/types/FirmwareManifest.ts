// Shape of Firmware/manifest.json as produced by tools/gen_firmware_manifest.py.

export interface FirmwareManifestFile {
    path: string;
    name: string;
    family: string;
    folder: string;
    date: string;
    size: number;
    sha256: string;
    rows: number;
    identity: string;
    version_code: number;
    version: string;
    serial_prefixes: string[];
    aliases?: string[];
}

export interface FirmwareManifest {
    schema: number;
    generated: string;
    repo: string;
    ref: string;
    commit: string | null;
    base_url: string;
    changelog_url: string;
    mapping_url: string;
    max_size: number;
    files: FirmwareManifestFile[];
}

export interface FirmwareBufferInfo {
    name: string;
    variant: string;
    source: 'psram' | 'ota' | 'none';
    size: number;
}
