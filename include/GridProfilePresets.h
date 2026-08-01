// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Curated catalogue of known-good grid profile blobs.
 *
 * IMPORTANT: entries here MUST come from a real capture off a working
 * inverter (either read via the DOWN_DAT / 0x8A response path or extracted
 * from a Hoymiles gateway session). Do NOT fabricate profile bytes: the
 * inverter validates the profile-id + version + section-shape and will
 * reject anything even slightly off, and a rejected write can leave the
 * inverter in a state that requires a Hoymiles service tool to recover.
 *
 * To add a real capture:
 *   1. Read the profile from the inverter using the existing
 *      GridOnProFilePara request (or capture on the wire).
 *   2. Strip the trailing 2-byte CRC16 the device appends to a read
 *      response (GridProfileWriteCommand computes and appends its own on
 *      write; keeping the read-side one bakes in a stale/duplicate CRC that
 *      gets double-counted and rejected by the inverter). Save the
 *      remaining content-only bytes as a static uint8_t array below.
 *   3. Add an entry to `kGridProfilePresets` with the human-friendly label.
 *
 * Until captures are added, the catalog is intentionally empty so the
 * WebAPI /api/gridprofile/knownprofiles endpoint returns [] and the UI
 * hides the "known profile" dropdown.
 */

struct GridProfilePreset {
    uint16_t id;              // Opaque identifier used by the WebAPI
    const char* label;        // Human-friendly name shown in the UI
    const uint8_t* data;      // Pointer to the raw profile bytes
    size_t dataLen;           // Length of the raw profile bytes
};

// Populate this array as real captures become available.
// Example (do not enable without a verified capture):
//   static constexpr uint8_t kDE_VDE4105_2018_bytes[] = { 0x03, 0x00, ... };
//   { 1, "DE - DE_VDE4105_2018", kDE_VDE4105_2018_bytes, sizeof(kDE_VDE4105_2018_bytes) },
extern const GridProfilePreset* const kGridProfilePresets;
extern const size_t kGridProfilePresetsCount;
