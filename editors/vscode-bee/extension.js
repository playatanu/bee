// Bee language support: completions, hovers, and light document symbol scanning.
// Written in plain JavaScript so it runs with no build step.
const vscode = require('vscode');

const KEYWORDS = [
  'let', 'fn', 'return', 'if', 'else', 'while', 'for', 'in', 'class', 'extends',
  'this', 'super', 'new', 'import', 'from', 'as', 'break', 'continue', 'and', 'or', 'not',
  'try', 'catch', 'finally', 'throw'
];
const CONSTANTS = ['true', 'false', 'nil'];

// [name, signature, docs]
const BUILTINS = [
  ['print', 'print(...values)', 'Print values separated by spaces, followed by a newline.'],
  ['write', 'write(...values)', 'Print values separated by spaces, with no trailing newline.'],
  ['len', 'len(x)', 'Length of a string, list, or dict.'],
  ['type', 'type(x)', 'Type name of a value, e.g. "number", "string", "list", or a class name.'],
  ['str', 'str(x)', 'Convert a value to its display string.'],
  ['repr', 'repr(x)', 'Quoted/debug string form of a value.'],
  ['num', 'num(x)', 'Parse a number from a string, bool, or number.'],
  ['int', 'int(x)', 'Truncate a number (or parsed string) toward zero.'],
  ['bool', 'bool(x)', 'Truthiness of a value (nil and false are falsey).'],
  ['abs', 'abs(n)', 'Absolute value.'],
  ['floor', 'floor(n)', 'Round down to an integer.'],
  ['ceil', 'ceil(n)', 'Round up to an integer.'],
  ['round', 'round(n)', 'Round to the nearest integer.'],
  ['sqrt', 'sqrt(n)', 'Square root.'],
  ['pow', 'pow(base, exp)', 'base raised to the power exp.'],
  ['min', 'min(a, b, ...)  |  min(list)', 'Smallest of the given numbers.'],
  ['max', 'max(a, b, ...)  |  max(list)', 'Largest of the given numbers.'],
  ['range', 'range(stop)  |  range(start, stop[, step])', 'Build a list of numbers.'],
  ['push', 'push(list, x)', 'Append x to a list; returns the list.'],
  ['pop', 'pop(list)', 'Remove and return the last element of a list.'],
  ['keys', 'keys(dict)', "List of a dict's keys."],
  ['values', 'values(dict)', "List of a dict's values."],
  ['ord', 'ord(s)', 'Character code of the first character of a string.'],
  ['chr', 'chr(n)', 'One-character string for code point n.'],
  ['input', 'input([prompt])', 'Read a line from standard input (nil at EOF).'],
  ['assert', 'assert(cond[, message])', 'Raise a runtime error if cond is falsey.'],

  // --- file I/O ---
  ['read_file', 'read_file(path)', 'Read a whole file as a string.'],
  ['read_lines', 'read_lines(path)', 'Read a file as a list of lines.'],
  ['write_file', 'write_file(path, text)', 'Write text to a file (truncates).'],
  ['append_file', 'append_file(path, text)', 'Append text to a file.'],
  ['file_exists', 'file_exists(path)', 'Whether the path exists.'],
  ['remove_file', 'remove_file(path)', 'Delete a file; returns whether it was removed.'],
  ['make_dir', 'make_dir(path)', 'Create a directory and its parents.'],
  ['list_dir', 'list_dir(path)', 'List entry names in a directory.'],

  // --- time / date ---
  ['clock', 'clock()', 'Monotonic seconds, for measuring elapsed time.'],
  ['time', 'time()', 'Seconds since the Unix epoch.'],
  ['now', 'now()', 'Local date/time as a dict (year, month, day, hour, ...).'],
  ['format_time', 'format_time(fmt[, epoch])', 'Format a time with strftime codes.'],
  ['sleep', 'sleep(seconds)', 'Pause; releases the lock so other threads run.'],

  // --- random ---
  ['random', 'random()', 'Float in [0, 1).'],
  ['random_int', 'random_int(a, b)', 'Integer in [a, b] inclusive.'],
  ['random_range', 'random_range(a, b)', 'Float in [a, b).'],
  ['random_choice', 'random_choice(list)', 'A random element of a list.'],
  ['random_seed', 'random_seed(n)', 'Seed the random generator.'],

  // --- environment / processes ---
  ['env', 'env(name[, default])', 'Environment variable, or default / nil.'],
  ['set_env', 'set_env(name, value)', 'Set an environment variable.'],
  ['args', 'args()', 'Command-line arguments after the script path.'],
  ['exec', 'exec(cmd)', 'Run a shell command; returns {code, output}.'],

  // --- threads ---
  ['spawn', 'spawn(fn[, ...args])', 'Start fn on a new thread; returns a handle.'],
  ['join', 'join(handle)', 'Wait for a thread and return its result.']
];

// Methods offered after a "." — grouped by receiver type. [name, signature]
const METHODS = {
  string: [
    ['len', 'len()'], ['upper', 'upper()'], ['lower', 'lower()'], ['trim', 'trim()'],
    ['contains', 'contains(s)'], ['starts_with', 'starts_with(s)'], ['ends_with', 'ends_with(s)'],
    ['split', 'split(sep)'], ['replace', 'replace(from, to)'], ['substr', 'substr(start, len)'],
    ['to_num', 'to_num()']
  ],
  list: [
    ['len', 'len()'], ['push', 'push(x)'], ['append', 'append(x)'], ['pop', 'pop()'],
    ['contains', 'contains(x)'], ['index_of', 'index_of(x)'], ['insert', 'insert(i, x)'],
    ['remove_at', 'remove_at(i)']
  ],
  dict: [
    ['keys', 'keys()'], ['values', 'values()'], ['has', 'has(key)'], ['remove', 'remove(key)'],
    ['len', 'len()'], ['get', 'get(key[, default])']
  ]
};

// Scan the document for user-declared names so they can be suggested.
function scanSymbols(doc) {
  const text = doc.getText();
  const out = new Map(); // name -> CompletionItemKind
  const add = (name, kind) => {
    if (name && !out.has(name)) out.set(name, kind);
  };
  let m;

  const fnRe = /\bfn\s+([A-Za-z_]\w*)/g;
  while ((m = fnRe.exec(text))) add(m[1], vscode.CompletionItemKind.Function);

  const clsRe = /\bclass\s+([A-Za-z_]\w*)/g;
  while ((m = clsRe.exec(text))) add(m[1], vscode.CompletionItemKind.Class);

  const letRe = /\blet\s+([A-Za-z_]\w*)/g;
  while ((m = letRe.exec(text))) add(m[1], vscode.CompletionItemKind.Variable);

  const forInRe = /\bfor\s+([A-Za-z_]\w*)\s+in\b/g;
  while ((m = forInRe.exec(text))) add(m[1], vscode.CompletionItemKind.Variable);

  const paramRe = /\bfn\s+[A-Za-z_]\w*\s*\(([^)]*)\)/g;
  while ((m = paramRe.exec(text))) {
    m[1].split(',').forEach((p) => {
      const n = p.trim();
      if (/^[A-Za-z_]\w*$/.test(n)) add(n, vscode.CompletionItemKind.Variable);
    });
  }
  return out;
}

const completionProvider = {
  provideCompletionItems(document, position) {
    const linePrefix = document.lineAt(position).text.slice(0, position.character);
    const items = [];

    // After a ".", suggest type methods (no type inference — offer the union).
    if (/\.[A-Za-z_]*$/.test(linePrefix)) {
      const seen = new Set();
      for (const group of Object.keys(METHODS)) {
        for (const [name, sig] of METHODS[group]) {
          const key = name + sig;
          if (seen.has(key)) continue;
          seen.add(key);
          const it = new vscode.CompletionItem(name, vscode.CompletionItemKind.Method);
          it.detail = `${group}.${sig}`;
          it.insertText = new vscode.SnippetString(name + '($0)');
          items.push(it);
        }
      }
      return items;
    }

    for (const k of KEYWORDS) {
      items.push(new vscode.CompletionItem(k, vscode.CompletionItemKind.Keyword));
    }
    for (const c of CONSTANTS) {
      items.push(new vscode.CompletionItem(c, vscode.CompletionItemKind.Constant));
    }
    for (const [name, sig, docs] of BUILTINS) {
      const it = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
      it.detail = sig;
      it.documentation = new vscode.MarkdownString(docs);
      it.insertText = new vscode.SnippetString(name + '($0)');
      items.push(it);
    }
    for (const [name, kind] of scanSymbols(document)) {
      items.push(new vscode.CompletionItem(name, kind));
    }
    return items;
  }
};

const hoverProvider = {
  provideHover(document, position) {
    const range = document.getWordRangeAtPosition(position, /[A-Za-z_]\w*/);
    if (!range) return;
    const word = document.getText(range);

    const b = BUILTINS.find((x) => x[0] === word);
    if (b) {
      const md = new vscode.MarkdownString();
      md.appendCodeblock(b[1], 'bee');
      md.appendMarkdown(b[2]);
      return new vscode.Hover(md, range);
    }
    if (KEYWORDS.includes(word)) {
      return new vscode.Hover(`**${word}** — Bee keyword`, range);
    }
    if (CONSTANTS.includes(word)) {
      return new vscode.Hover(`**${word}** — Bee literal`, range);
    }
  }
};

function activate(context) {
  const selector = { language: 'bee' };
  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(selector, completionProvider, '.'),
    vscode.languages.registerHoverProvider(selector, hoverProvider)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
