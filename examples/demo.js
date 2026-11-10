// qtmonaco-native javascript highlighting demo — open with:
//   ./build/src/app/qmn-spike examples/demo.js
/* Block comments, template literals, regex literals, classes and modern
   syntax all ride the upstream javascript highlights.scm capture table. */

import { readFile } from "node:fs/promises"; // ES module import
export const VERSION = "1.0.0-rc.1";

const HEX = 0xff0d, MASK = 0b1010_1010, EXP = 1.25e3, BIG = 9_007_199_254_740_991n;
let count = -42, flag = true, nothing = null, undef = undefined, inf = Infinity;

/** JSDoc is just a block comment to the grammar. */
class TokenCounter {
  static builtinNames = new Set(["console", "process"]);

  #pattern = /^\/\/.*$/gm; // private field + regex literal

  constructor(name) {
    this.name = name;
    this.spans = [];
  }

  add(start, end, kind = "text") {
    if (start >= end) return; // guard clause
    this.spans.push({ start, end, kind: kind === "text" ? kind : `token:${kind}` });
  }

  total = () => this.spans.length; // arrow assigned as a method
}

async function summarize(source) {
  const counter = new TokenCounter("demo");
  for (const line of source.split("\n")) {
    const trimmed = line.trim();
    if (trimmed.startsWith("//")) counter.add(0, line.length, "comment");
    else if (trimmed.length) counter.add(0, line.length);
  }
  try {
    const info = await readFile(import.meta.url);
    console.debug(`scanned ${counter.total()} lines, ${info.size ?? "?"} bytes`);
  } catch (err) {
    console.error(err.message);
    throw err;
  }
  return counter.spans;
}

const [, , ...rest] = process.argv; // spread + destructuring
export default summarize;
