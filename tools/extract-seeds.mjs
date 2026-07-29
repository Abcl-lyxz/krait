// Extracts every corpus IN payload into raw seed files (ADR-0010: "seeds =
// every corpus IN payload"). Node built-ins only. Usage:
//   node tools/extract-seeds.mjs <outDir>
import { mkdirSync, readdirSync, readFileSync, writeFileSync, copyFileSync } from "node:fs";
import { join, basename } from "node:path";

const outDir = process.argv[2];
if (!outDir) {
    console.error("usage: node tools/extract-seeds.mjs <outDir>");
    process.exit(1);
}
mkdirSync(outDir, { recursive: true });

function parseBytes(spec) {
    const bytes = [];
    for (let i = 0; i < spec.length; ) {
        if (spec[i] === "\\" && spec[i + 1] === "x" && i + 3 < spec.length) {
            bytes.push(parseInt(spec.slice(i + 2, i + 4), 16));
            i += 4;
        } else {
            bytes.push(spec.charCodeAt(i));
            i += 1;
        }
    }
    return Buffer.from(bytes);
}

let count = 0;
const corpusRoot = "tests/corpus";
for (const dir of readdirSync(corpusRoot, { withFileTypes: true })) {
    if (!dir.isDirectory()) continue;
    for (const file of readdirSync(join(corpusRoot, dir.name))) {
        if (!file.endsWith(".case")) continue;
        // latin1: one byte = one code unit, so raw UTF-8 payloads in .case
        // files survive charCodeAt byte-exactly (utf8 would decode them).
        const lines = readFileSync(join(corpusRoot, dir.name, file), "latin1").split("\n");
        let n = 0;
        for (const line of lines) {
            const trimmed = line.replace(/\r$/, "");
            if (!trimmed.startsWith("IN ")) continue;
            const name = `${dir.name}-${basename(file, ".case")}-${n++}.bin`;
            writeFileSync(join(outDir, name), parseBytes(trimmed.slice(3)));
            count++;
        }
    }
}
for (const file of readdirSync("tests/fuzz/seeds")) {
    if (!file.endsWith(".seed")) continue;
    copyFileSync(join("tests/fuzz/seeds", file), join(outDir, file));
    count++;
}
console.log(`extracted ${count} seeds -> ${outDir}`);
