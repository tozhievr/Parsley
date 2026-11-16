import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
execFileSync(process.execPath,[path.join(root,'node_modules/typescript/bin/tsc'),'-p',root],{stdio:'inherit'});
const output=path.join(root,'out/extension.js');
fs.writeFileSync(output,fs.readFileSync(output,'utf8').replace(/\r\n/g,'\n').replace(/[ \t]+$/gm,''));