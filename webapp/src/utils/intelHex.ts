// Browser-side Intel-Hex validation for inverter firmware images.
//
// The DTU transmits every ':' line of the file as one row (see
// IntelHex::decodeRow in lib/Hoymiles) and only notices a corrupt line while
// the flash is already running. Checking the per-line checksums, the byte
// count fields and the EOF record here catches truncated or wrong files before
// a single byte is sent to the DTU. This applies to downloads and local files
// alike.

// MAX_FIRMWARE_UPLOAD_SIZE in src/WebApi_file.cpp (raw ASCII bytes).
export const MAX_FIRMWARE_UPLOAD_SIZE = 800 * 1024;

export type IntelHexErrorCode = 'prefix' | 'hex' | 'length' | 'checksum' | 'no_eof' | 'empty';

export interface IntelHexInfo {
    rows: number; // rows the DTU will transmit (incl. identity and EOF rows)
    hasEof: boolean;
    identity: string; // 4 identity bytes as upper-case hex, '' if the first row is no identity row
    versionCode: number; // 0 if unknown
    version: string; // decoded version, '' if unknown
}

export type IntelHexValidation =
    | { ok: true; info: IntelHexInfo }
    | { ok: false; error: IntelHexErrorCode; line: number; info: IntelHexInfo };

// Hoymiles encodes the version as decimal MMmmpp: 0x2904 = 10500 -> 1.5.0.
export function formatFirmwareVersion(code: number): string {
    return `${Math.floor(code / 10000)}.${Math.floor(code / 100) % 100}.${code % 100}`;
}

function nibble(c: number): number {
    if (c >= 0x30 && c <= 0x39) return c - 0x30;
    if (c >= 0x41 && c <= 0x46) return 10 + c - 0x41;
    if (c >= 0x61 && c <= 0x66) return 10 + c - 0x61;
    return -1;
}

export function validateIntelHex(text: string): IntelHexValidation {
    const info: IntelHexInfo = { rows: 0, hasEof: false, identity: '', versionCode: 0, version: '' };
    const lines = text.split('\n');

    for (let index = 0; index < lines.length; index++) {
        const lineNo = index + 1;
        const line = (lines[index] ?? '').trim();
        if (line.length === 0) {
            continue;
        }
        if (info.hasEof) {
            // The DTU stops reading after the EOF row, so trailing content is
            // harmless and ignored here as well.
            break;
        }
        const fail = (error: IntelHexErrorCode): IntelHexValidation => ({ ok: false, error, line: lineNo, info });

        if (line.charCodeAt(0) !== 0x3a) {
            return fail('prefix');
        }
        const body = line.length - 1;
        if (body < 10 || body % 2 !== 0) {
            return fail('length');
        }

        const total = body / 2;
        const bytes = new Uint8Array(total);
        let sum = 0;
        for (let i = 0; i < total; i++) {
            const hi = nibble(line.charCodeAt(1 + 2 * i));
            const lo = nibble(line.charCodeAt(2 + 2 * i));
            if (hi < 0 || lo < 0) {
                return fail('hex');
            }
            const value = (hi << 4) | lo;
            bytes[i] = value;
            sum += value;
        }

        // byte count field + 2 address + 1 record type + 1 checksum
        const byteCount = bytes[0] ?? -1;
        if (byteCount !== total - 5) {
            return fail('length');
        }
        if ((sum & 0xff) !== 0) {
            return fail('checksum');
        }

        info.rows++;
        const recordType = bytes[3];
        if (recordType === 0x01) {
            info.hasEof = true;
        }
        if (info.rows === 1 && recordType === 0x11 && total >= 11) {
            // Vendor identity row: [4..7] identity nibbles, [8..9] version (BE).
            info.identity = Array.from(bytes.subarray(4, 8))
                .map((b) => b.toString(16).toUpperCase().padStart(2, '0'))
                .join('');
            info.versionCode = ((bytes[8] ?? 0) << 8) | (bytes[9] ?? 0);
            info.version = formatFirmwareVersion(info.versionCode);
        }
    }

    if (info.rows === 0) {
        return { ok: false, error: 'empty', line: 0, info };
    }
    if (!info.hasEof) {
        return { ok: false, error: 'no_eof', line: lines.length, info };
    }
    return { ok: true, info };
}

// Maps a failed validation to an i18n key below "firmwaresource" plus its
// interpolation parameters, so views can render it with $t().
export function intelHexErrorMessage(v: Extract<IntelHexValidation, { ok: false }>): {
    key: string;
    params: Record<string, string | number>;
} {
    const keys: Record<IntelHexErrorCode, string> = {
        prefix: 'HexErrorPrefix',
        hex: 'HexErrorHex',
        length: 'HexErrorLength',
        checksum: 'HexErrorChecksum',
        no_eof: 'HexErrorNoEof',
        empty: 'HexErrorEmpty',
    };
    return { key: keys[v.error], params: { line: v.line } };
}
