// Shape of Firmware/manifest.json as produced by tools/gen_firmware_manifest.py.

export interface FirmwareManifestFile {
    path: string; // relative to the manifest's base_url, e.g. "HM/HM_1T_1_5_0.hex"
    name: string; // file name without extension
    family: string; // top-level folder: HM, HMS, HMT, MIT, ...
    folder: string; // folder below Firmware/, '' for the root
    date: string; // ISO 8601 commit date of the file
    size: number; // byte count of the ASCII .hex file
    sha256: string;
    rows: number; // Intel-Hex rows the DTU will transmit
    identity: string; // the four identity bytes as upper-case hex
    version_code: number;
    version: string; // decoded firmware version, e.g. "1.5.0"
    serial_prefixes: string[]; // upper-case hex serial prefixes this image is valid for
    aliases?: string[]; // other paths holding a byte-identical image
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

// Response of GET /api/file/firmware_info: what the DTU currently buffers.
export interface FirmwareBufferInfo {
    name: string;
    variant: string;
    source: 'psram' | 'ota' | 'none';
    size: number;
}
