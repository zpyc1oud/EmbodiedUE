# Map reference

The map is one issue on the configured tracker, labeled `wayfinder:map`. Its child issues are decision tickets. The map is an index: it records the destination, standing notes, a one-line gist and link for each resolved ticket, and the current in-scope fog. Ticket bodies hold the decision detail.

## Map body

```markdown
## Destination

<what reaching the end of this map looks like>

## Notes

<domain, skills, and standing preferences>

## Decisions so far

- [<closed ticket title>](link) — <one-line gist>

## Not yet specified

<in-scope questions that are not sharp enough to ticket>

## Out of scope

<work ruled beyond the destination>
```

Each ticket asks one decision or investigation and carries a `wayfinder:<type>` label. Its issue identity is the tracker id. Claim it by assigning it to the driving developer before work. Use native blocking relationships when available; otherwise follow the configured body convention. The frontier is open, unblocked, unclaimed child tickets.

Record a resolution in the ticket, close it, and append a short context pointer to the map's Decisions-so-far. Link artifacts from the ticket instead of pasting them into the map.
