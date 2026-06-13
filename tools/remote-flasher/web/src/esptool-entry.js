// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Entry point for the esptool-js bundle. The Espressif library is published
// only as an ES module, so we re-export the bits we need on a global
// `window.EsptoolJS` namespace and let `bun build --format=iife` wrap it.
//
// Keep the export surface small: index.html only needs ESPLoader / Transport.

import { ESPLoader, Transport } from "esptool-js";

if (typeof window !== "undefined") {
    window.EsptoolJS = { ESPLoader, Transport };
}

export { ESPLoader, Transport };
