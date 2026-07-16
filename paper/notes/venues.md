# Venue plan (updated 2026-07-16 — re-verify dates before acting)

## Strategy update (2026-07-16): deepen-the-platform

A second physical platform is not available for now, so the paper's identity
is a *definitive single-system case study*: observed cluster residency +
energy per op (powermetrics), industrial cross-check arms (rigtorp MPMC,
xenium MS+hazard-pointers), sustained thermal characterization, bootstrap
CIs. Venue ladder reordered accordingly:

1. **ICPE 2027** (research track; LTB workshop as safety net) — best fit for
   the methodology/artifact strengths; single-system studies are normal there.
2. **JSys** — artifact-mandatory rolling journal; welcomes case studies and
   negative results. Can run in parallel with a conference plan? Check their
   concurrent-submission policy first.
3. **PMAM@PPoPP / ISPASS poster or short paper** — fallbacks.
4. **ISPASS/IISWC main track** — after a second platform (ideally Alder Lake:
   P/E cores AND affinity) reruns the core matrix.

## Timing reality (as of July 2026)

| Venue | Fit | Status / expected deadline |
|---|---|---|
| **ISPASS 2027** | Best fit — IEEE performance analysis of systems/software; measurement studies are the core genre | ISPASS 2026's deadline was Dec 8/15 2025 (abstract/full), so **expect ~Dec 2026** for 2027. This is the primary target, ~5 months of polish time. Watch https://ispass.org |
| IISWC 2027 | Workload/system characterization; single-platform deep dives welcome | IISWC 2026's deadline (May 14 2026) has passed; next cycle **~May 2027**. Good backup / second shot. https://iiswc.org |
| ICPE 2027 (ACM/SPEC) | Performance engineering + benchmarking methodology; artifact-friendly | CFP not yet out; ICPE historically deadlines **~Oct** for a spring conference — check https://icpe2026.spec.org / icpe.spec.org in early fall 2026. Would need acmart reformat. |
| arXiv | Timestamp + feedback | Anytime — **after the author rewrite pass**, not before. |

## Recommended sequence

1. **Now → Sept 2026:** author rewrite pass (own voice, verify every number),
   run the cross-platform CI workflow and fold in the directional data,
   optional k=10 full-matrix refresh.
2. **~Sept 2026:** arXiv preprint.
3. **~Oct 2026:** if ICPE 2027 CFP fits, submit there (reformat to acmart);
   otherwise
4. **~Dec 2026:** ISPASS 2027 (primary).
5. **~May 2027:** IISWC 2027 if earlier attempts miss.

## Submission checklist (venue-independent)

- [ ] Author rewrite pass complete; every number re-verified against CSVs
- [ ] AI-use disclosure per venue policy (IEEE and ACM both have policies —
      read the current text at submission time)
- [ ] Affiliation line resolved (currently placeholder)
- [ ] Cross-platform CI data integrated (or the claim scoped to M2 only)
- [ ] Page limit check (ISPASS: typically 10-12pp incl. refs for full papers;
      verify in the CFP)
- [ ] Artifact appendix / ARTIFACT.md aligned with the venue's AE process
- [ ] refs.bib entries verified against DBLP (titles/years/pages)

Sources checked: [ISPASS 2026 submissions](https://ispass.org/ispass2026/submission.php) ·
[IISWC 2026 CFP](https://iiswc.org/iiswc2026/cfp.html) ·
[ICPE 2026 site](https://icpe2026.spec.org/)
