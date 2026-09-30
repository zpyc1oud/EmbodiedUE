# UE RL Engine

在 Unreal Engine 5.8 Chaos 中训练机器人策略，并把同一份制品部署进游戏里的 Skeletal Mesh。

- 怎么跑、目录和命令：`README.md`
- 领域术语：`CONTEXT.md`
- 系统如何工作：`docs/UE-RL-Engine-项目说明文档.html`
- 文档地图：`docs/README.md`
- How-to（新机器人、录屏、接到其它工程）：`docs/how-to/README.md`

接入一台新机器人只增加 UE 资产、Python 机器人/任务声明和训练制品，不增加机器人专用 UE runtime。产品机器人是 CartPole 与 PhantomX。训练、评测、导出和接到其它工程都走 `uerl`。Session 持有 `physics_dt` 与闭区间 `decimation`；每个 Step 另带该区间内的 `step_decimation`。接触观测是控制窗口末、最后一个已完成 solver step 的几何支撑 `0/1`。

=== SCOPE LIMITS (these bound what you PROPOSE, never what you look for) ===
Report anything that is actually wrong here — including a rare-looking case, if
this project actually produces it. Then keep the fix in scope:
1. This is not a security paper. Verification is welcome; over-defense is not.
   Unless this project states otherwise, assume a cooperating operator on their
   own machine; if it has a real adversary, it will say so and that scope wins.
2. Do not add hashes, checksums or fingerprints unless the hash replaces a
   materially more expensive operation AND its result changes what happens next.
3. No defensive scaffolding: no feature flags, migration frameworks, compat
   layers or wrappers for cases that do not occur here.
4. No corner-case obsession: exotic encodings, symlink races, RTL text and
   millisecond races are out of scope unless the case is reachable through this
   project's supported use — its documented inputs, its published interface, its
   real data. Reachable is enough; you do not need a reproduction. Constructible
   in principle is not enough.
5. Where judgement is needed, judge. Do not replace it with a scoring table, a
   checklist, or a re-verification loop over something already settled.
6. None of this overrides security, migration, verification or review that the
   user, this project's own conventions, or a higher-priority rule asked for.
   Those were requested; they are the work, not scope creep.
7. Deliverable text is not a defense transcript. State plainly what holds;
   collect caveats in one section (Limitations, Known Issues) instead of
   sprinkling a disclaimer into every paragraph; and never write instructions
   into the product: "do not mention X" means X is absent — not "we do not
   address X".
Shapes already seen, for calibration. Examples, not a checklist — a real finding
is not dismissed by resembling one:
  H  hashing every row of two spreadsheets to answer what comparing cells answers
  H  writing checksum files that nothing ever reads
  E  hardening the accounts of an app that has no users and no deployment
  R  auditing your own patch all night while the feature stays unwritten
  R  a reviewer that returns a failing verdict on everything
  O  guards whose justification is the previous guard, not the requirement
And two that look like the above and are not. Report these:
  ✓  a digest that lets you skip re-reading a large file you already have
  ✓  a rare-looking input this project's own documentation example produces
Before running any check, answer: what specific failure would this detect, and
what would I do differently if it occurred? No answer means do not run it.
Say plainly when something is correct. Do not manufacture findings.
