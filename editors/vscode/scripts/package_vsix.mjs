#!/usr/bin/env node
"use strict";

/**
 * Zero-dependency cross-platform OpenVSIX (.vsix) packager for Parsley VSCode/Cursor extension.
 * Uses only Node.js built-ins (fs, path, zlib).
 */

import fs from "fs";
import path from "path";
import zlib from "zlib";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const EXTENSION_ROOT = path.resolve(__dirname, "..");

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) {
      c = (c & 1) !== 0 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[i] = c >>> 0;
  }
  return table;
})();

function crc32(buf) {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc = CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function toDosTimeDate(date) {
  const year = Math.max(1980, date.getFullYear());
  const month = date.getMonth() + 1;
  const day = date.getDate();
  const hours = date.getHours();
  const minutes = date.getMinutes();
  const seconds = Math.floor(date.getSeconds() / 2);
  const dosTime = (hours << 11) | (minutes << 5) | seconds;
  const dosDate = ((year - 1980) << 9) | (month << 5) | day;
  return { dosTime, dosDate };
}

function buildZipBuffer(entries) {
  const localChunks = [];
  const centralChunks = [];
  let offset = 0;

  for (const entry of entries) {
    const nameBuf = Buffer.from(entry.name, "utf8");
    const rawBuf = entry.data;
    const deflated = zlib.deflateRawSync(rawBuf, { level: 9 });
    const useDeflate = deflated.length < rawBuf.length;
    const compBuf = useDeflate ? deflated : rawBuf;
    const method = useDeflate ? 8 : 0;
    const crc = crc32(rawBuf);
    const { dosTime, dosDate } = toDosTimeDate(entry.mtime || new Date());

    // Local File Header (30 bytes + filename)
    const local = Buffer.allocUnsafe(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4); // version needed
    local.writeUInt16LE(0x0800, 6); // UTF-8 filename flag
    local.writeUInt16LE(method, 8);
    local.writeUInt16LE(dosTime, 10);
    local.writeUInt16LE(dosDate, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(compBuf.length, 18);
    local.writeUInt32LE(rawBuf.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28); // extra len

    localChunks.push(local, nameBuf, compBuf);

    // Central Directory Header (46 bytes + filename)
    const central = Buffer.allocUnsafe(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE((3 << 8) | 20, 4); // Unix + 2.0
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8); // UTF-8 filename flag
    central.writeUInt16LE(method, 10);
    central.writeUInt16LE(dosTime, 12);
    central.writeUInt16LE(dosDate, 14);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(compBuf.length, 20);
    central.writeUInt32LE(rawBuf.length, 24);
    central.writeUInt16LE(nameBuf.length, 28);
    central.writeUInt16LE(0, 30); // extra len
    central.writeUInt16LE(0, 32); // comment len
    central.writeUInt16LE(0, 34); // disk start
    central.writeUInt16LE(0, 36); // internal attrs
    central.writeUInt32LE((0o100644 << 16) >>> 0, 38); // external attrs
    central.writeUInt32LE(offset, 42);

    centralChunks.push(central, nameBuf);
    offset += local.length + nameBuf.length + compBuf.length;
  }

  const centralDirBuf = Buffer.concat(centralChunks);
  const eocd = Buffer.allocUnsafe(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralDirBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);
  eocd.writeUInt16LE(0, 20);

  return Buffer.concat([...localChunks, centralDirBuf, eocd]);
}

function escapeXml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&apos;");
}

function collectRuntimeFiles(dir, relPrefix = "") {
  const results = [];
  const excludedTopDirs = new Set([".vscode", "src", "scripts", "node_modules"]);
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  entries.sort((a, b) => a.name.localeCompare(b.name));

  for (const item of entries) {
    const rel = relPrefix ? `${relPrefix}/${item.name}` : item.name;
    const fullPath = path.join(dir, item.name);

    if (item.isDirectory()) {
      if (!relPrefix && excludedTopDirs.has(item.name)) {
        continue;
      }
      results.push(...collectRuntimeFiles(fullPath, rel));
      continue;
    }

    if (
      item.name === "tsconfig.json" ||
      item.name.endsWith(".vsix") ||
      item.name.endsWith(".ts") ||
      item.name.endsWith(".map")
    ) {
      continue;
    }

    const stat = fs.statSync(fullPath);
    results.push({
      relPath: rel,
      fullPath,
      mtime: stat.mtime,
    });
  }
  return results;
}

function buildContentTypesXml() {
  return (
    `<?xml version="1.0" encoding="utf-8"?>\n` +
    `<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n` +
    `  <Default Extension="json" ContentType="application/json"/>\n` +
    `  <Default Extension="js" ContentType="application/javascript"/>\n` +
    `  <Default Extension="md" ContentType="text/markdown"/>\n` +
    `  <Default Extension="png" ContentType="image/png"/>\n` +
    `  <Default Extension="svg" ContentType="image/svg+xml"/>\n` +
    `  <Default Extension="vsixmanifest" ContentType="text/xml"/>\n` +
    `  <Default Extension="code-snippets" ContentType="application/json"/>\n` +
    `  <Default Extension="txt" ContentType="text/plain"/>\n` +
    `</Types>\n`
  );
}

function buildVsixManifestXml(pkg) {
  const publisher = escapeXml(pkg.publisher || "parsley-lang");
  const id = escapeXml(pkg.name || "parsley-lang");
  const version = escapeXml(pkg.version || "1.6.0");
  const displayName = escapeXml(pkg.displayName || id);
  const description = escapeXml(pkg.description || "");
  const engine = escapeXml((pkg.engines && pkg.engines.vscode) || "^1.85.0");
  const tags = escapeXml((pkg.keywords || []).join(","));
  const categories = escapeXml((pkg.categories || []).join(","));
  const iconRel = pkg.icon ? `extension/${pkg.icon.replace(/^\.\//, "")}` : "";
  const iconElement = iconRel ? `\n    <Icon>${escapeXml(iconRel)}</Icon>` : "";
  const iconAsset = iconRel
    ? `\n    <Asset Type="Microsoft.VisualStudio.Services.Icons.Default" Path="${escapeXml(iconRel)}" Addressable="true"/>`
    : "";

  return (
    `<?xml version="1.0" encoding="utf-8"?>\n` +
    `<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">\n` +
    `  <Metadata>\n` +
    `    <Identity Language="en-US" Id="${id}" Version="${version}" Publisher="${publisher}"/>\n` +
    `    <DisplayName>${displayName}</DisplayName>\n` +
    `    <Description xml:space="preserve">${description}</Description>\n` +
    `    <Tags>${tags}</Tags>\n` +
    `    <Categories>${categories}</Categories>${iconElement}\n` +
    `    <Properties>\n` +
    `      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="${engine}"/>\n` +
    `    </Properties>\n` +
    `  </Metadata>\n` +
    `  <Installation>\n` +
    `    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>\n` +
    `  </Installation>\n` +
    `  <Dependencies/>\n` +
    `  <Assets>\n` +
    `    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>\n` +
    `    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true"/>${iconAsset}\n` +
    `  </Assets>\n` +
    `</PackageManifest>\n`
  );
}

function main() {
  const pkgPath = path.join(EXTENSION_ROOT, "package.json");
  const pkg = JSON.parse(fs.readFileSync(pkgPath, "utf8"));
  const version = pkg.version || "1.6.0";
  const name = pkg.name || "parsley-lang";

  const runtimeFiles = collectRuntimeFiles(EXTENSION_ROOT);
  const now = new Date();

  const zipEntries = [
    {
      name: "[Content_Types].xml",
      data: Buffer.from(buildContentTypesXml(), "utf8"),
      mtime: now,
    },
    {
      name: "extension.vsixmanifest",
      data: Buffer.from(buildVsixManifestXml(pkg), "utf8"),
      mtime: now,
    },
    ...runtimeFiles.map((file) => ({
      name: `extension/${file.relPath}`,
      data: fs.readFileSync(file.fullPath),
      mtime: file.mtime,
    })),
  ];

  const vsixBuffer = buildZipBuffer(zipEntries);
  const versionedOut = path.join(EXTENSION_ROOT, `${name}-${version}.vsix`);
  const unversionedOut = path.join(EXTENSION_ROOT, `${name}.vsix`);

  fs.writeFileSync(versionedOut, vsixBuffer);
  fs.writeFileSync(unversionedOut, vsixBuffer);

  console.log(
    `Packaged ${zipEntries.length} entries (${vsixBuffer.length} bytes) -> ${path.basename(versionedOut)} and ${path.basename(unversionedOut)}`
  );
}

main();
