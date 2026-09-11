# Documentation standards

How the project writes prose: docs, comments, and the pages generated from them. Code rules are in [coding-standards.md](coding-standards.md). The two meet at `///` comments, which are code by location and documentation by purpose: their syntax is a coding-standards rule, everything they say follows this page.

Every rule has one home. Another document links here rather than restating, because two copies become two different rules.

## What we document

Every page is one of these, in the order a newcomer meets them.

| Page | Holds | Written by |
|---|---|---|
| **README.md** | what projectMM is, and first light in under a minute | hand |
| **CLAUDE.md** | the rules: principles, process, roles | hand |
| **architecture.md** | how the system fits together, and why it is shaped that way | hand |
| **Summary pages** | one row per module: what it is, its controls, its links | hand |
| **Technical pages** | every class, member and control, from the `.h` | generated |
| **Tutorials and use cases** | one task, start to finish | hand |
| **Standards** | how we write code and documentation | hand |
| **work/future** | what does not exist yet: the backlog | hand |
| **work/present** | plans being built now, deleted at their PR | hand |
| **work/past** | shipped plans and dated records | hand, editable and prunable |

The split that matters: a hand-written page says what the code cannot, and a generated page IS the code. Nobody edits a generated page, because the next build overwrites it.

**A shipped plan is a working document, not testimony.** It may be edited, trimmed, or deleted: whatever its PR already carries is duplication. Only a dated record stating what was true at a moment (release notes, an inventory quoted from another project) is kept unrewritten.

Document a thing once, in the place closest to it, and link the rest. A fact the source states is never re-typed in prose.

## Writing

- **A page is read start to finish by one reader**, either a **user** (no coding, no hardware knowledge beyond plugging in a board) or a **developer** (C++, embedded, this codebase's shape). Where a page serves both, lead with the user and put the depth lower down.
- **One tone of voice, everywhere: factual, no nonsense.** State what is true and what to do, addressing the reader as "you". Leave out enthusiasm, apology, and how we felt building it. Only the assumed knowledge changes between pages, never the voice.
- **Follow the [principles](../CLAUDE.md#principles).** Three bear on documentation directly:
    - **Minimalism**: every fact has one home; history lives in git.
        - **Present tense only.** "No X anymore" narrates a removal, which is history. Describe the path that exists today.
        - **Positive form only.** "Not", "never", "neither", "without", "un-" and "non-" are the alarm bells: a negation says everything a thing is not, which is no shape at all. A real constraint stays ("the DMA cannot read PSRAM at shift clock"); a bare absence goes.
    - **Industry standards**: the textbook name for a thing, so a reader recognizes it without being taught our vocabulary. A bespoke choice carries its one-line reason where it appears.
    - **Continuous improvement**: a doc describing what the code no longer does is a defect. Fix it in the change that opened the file, not in a sweep.
- **Say it, then stop.** One or two sentences a reader can act on. A reason earns one more when the point is surprising or has been got wrong before; past that it is an argument, and an argument is not documentation.
- **About 40 words per statement.** Past that a reader skims, and a skimmed statement is not followed.
- **Write for the reader who will follow it**, not the one arguing with it. A trap that only bites whoever maintains the tooling belongs in the tooling.
- **Mechanism lives with the mechanism.** How a generator or script works belongs in that script. A page says what the reader must do.
- **One parenthetical per sentence.** A second qualification means the sentence carries two ideas: split it, or drop the weaker one.
- **American English spelling, everywhere**: identifiers, wire keys, comments, docs, UI strings. A grep for one dialect silently misses the other, and a drifting wire key breaks a contract with no compile error. A proper noun keeps its own spelling.
- **No em-dashes in prose.** Use a comma, colon, parentheses, or a full stop. A literal one in a UI string or test fixture stays.
- **No hard line wraps in markdown.** Let the editor soft-wrap, so a one-word edit is a one-word diff.
- **Convert as you touch.** Spelling and em-dash fixes ride the change that opens the file, never a repo-wide sweep. `check_prose.py` checks added lines only, for the same reason.

## Module pages

### Two surfaces per module

1. **A summary page**, hand-written, for the end user. One table row in its group's page, carrying the module's description, image, controls, and links. Catalog controls live here because they are runtime `controls_.add(...)` calls that no static tool sees.
2. **A technical page**, generated from the header's `///` comments by [`moondeck/docs/gen_api.py`](../moondeck/docs/gen_api.py).

`docs/moonmodules/` mirrors `src/`: a `core/` and a `light/` subtree, one flat page per group. The tree is `<core|light>/<type>/Module`, flat within a type, and **library is a tag, not a folder level**: in code and assets it rides in `tags()`, in docs it rides in the page name, so `effects.md` splits to `effects_<library>.md` only when a section outgrows it. A blended-lineage module then needs no file move, and a mis-filing is a one-line edit. Generated pages are gitignored and rebuilt. Test inventories have their own generator, [`generate_test_docs.py`](../moondeck/docs/generate_test_docs.py), because unit tests are macros and scenarios are JSON.

**Reference a module generically** outside its own home: say "a modifier", not `FooModifier`. A module's home is its `.h`, its catalog card, its registration, and its tests. Naming it elsewhere multiplies rename cost.

**No per-module detail page.** Cross-file rationale that no single `.h` owns goes in a prose section under its group's summary page. Rationale shared by sibling modules lives once on their base class.

## Comments

- **Comments say WHY.** Restating what the line does is noise, and usually a naming failure: see [prefer naming over commenting](coding-standards.md#conventions).
- **One line, above the code it explains.** A second line is the drift signal: the first line said the thing, and the rest is the author still talking. Class descriptions are the exception, and they have the budget below.
- **A budget, in lines.** A class `///` is about 10 lines, an `@moreinfo` appendix about 20. Over budget, cut. A file whose comments outnumber its code has stopped being a header.
- **Keep the constraint, cut the exposition.** A constraint cannot be recovered from the code: the latch is 300 us, the DMA cannot read PSRAM at shift clock. What was tried first, and why this pattern over another, goes in the commit message.
- **Removing a comment needs the same justification as removing code**: outdated, wrong, or it only restated the code. Never strip to hit a length target, and never delete a reason you cannot reconstruct.
- **Say each fact once.** A restatement for emphasis reads as new information and costs the reader a second pass to learn it is not.
- **A heading inside a comment means it is not a comment.** Needing signposts is the signal to cut. The one exception is `@moreinfo`, whose `##` sections become the generated page's own headings; inside a lead comment a heading wants to be a page, or wants to not exist.
- **MoonLive scripts get three comments, one line each**: what the script is, what each `addControl` knob does, what each function it defines does. Lifecycle functions need none. A script is read in the device's own editor, where prose buries the effect.

### Writing a `///` that generates correctly

These are traps, not style: each one silently loses content from the generated page.

- **`///`, not `//`.** Only `///` generates. A `//` comment is invisible on the technical page.
- **The class `///` sits directly above `class X`**, inside the namespace. Separated by includes or the namespace open, Doxygen drops it and the page loses its description.
- **Put detail on the member it describes**, not in the class comment. Every public control and method gets its own `///` leading with one sentence, which is what the generated page shows as its summary.
- **Deep dives go after `@moreinfo`** at the end of the class block, and a post-process moves them below the member lists. The budget above applies: this is not an unbounded appendix.
- **No relative `.md` links.** Doxygen keeps only `http(s)://` links. For an in-page link use `@xref{anchor|label}`.
- **Wrap any `<tag>` in backticks**, or it renders as a live element and swallows the page.
- **Write "such as", not "e.g."** The brief ends at the first period.
