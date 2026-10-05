# UE RL Engine

Train robot policies in Unreal Engine 5.8 Chaos and deploy the same artifact on a game Skeletal Mesh.

- Read `README.md` for installation, commands, and the directory layout.
- Read `CONTEXT.md` when changing domain contracts or timing semantics.
- Read `docs/architecture.md` for system boundaries and design rationale.
- Use `docs/README.md` to locate topic-specific documentation.
- Follow `docs/contribution-workflow.md` for proposals, validation evidence, review, merge, and release; it does not grant action permissions beyond the user's authorization.
- Read `docs/how-to/README.md` for robot integration, recording, and deployment workflows.

Integrating a robot adds UE assets, Python robot/task declarations, and a policy artifact; it does not add robot-specific UE runtime code. The product robots are CartPole and PhantomX. Training, evaluation, export, and deployment use `uerl`. A Session owns `physics_dt` and an inclusive `decimation` range; each Step carries a `step_decimation` inside that range. Contact observations are geometric support values (0/1) from the final completed solver step at the end of the control window.

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
