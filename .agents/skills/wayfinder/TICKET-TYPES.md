# Ticket types

Classify each decision ticket as HITL or AFK according to who must supply the answer. A HITL ticket needs a live human exchange; the agent does not answer for the human.

- **Research (AFK):** read documentation, third-party APIs, or local resources. Resolve with `/research` and link the cited findings.
- **Prototype (HITL):** build a cheap artifact that makes a UI, behavior, or data-model choice concrete. Use `/prototype` and link it.
- **Grilling (HITL):** resolve a decision through conversation. Use `/grilling` and `/domain-modeling`.
- **Task (HITL or AFK):** perform manual work that unblocks a later decision, such as provisioning access or moving data for inspection. It does not deliver the destination.
