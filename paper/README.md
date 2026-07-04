# paper/ — working directory for the MPMC characterization study

Target shape: a short measurement/characterization paper, IEEE two-column
(ISPASS / IISWC / similar small venue), with an arXiv preprint and a
reproducibility artifact (this repo).

**Working title:** *MPMC Queue Tradeoffs on Asymmetric Client Silicon:
A Characterization Study on Apple M2*

## Layout

```
main.tex        IEEEtran source (Phase E; compiled with tectonic)
refs.bib        bibliography
notes/          the thinking: claims.md is the source of truth for every claim
data/           committed benchmark CSVs (the artifact's raw results)
assets/         generated figures (scripts/make_plots.py output)
```

## Ground rules (read before editing)

1. **claims.md is the contract.** No sentence goes into main.tex that isn't
   backed by an entry in `notes/claims.md` with data behind it. Scope every
   claim: one machine, client silicon, characterization — never "queue X is
   best."
2. **Authorship & AI disclosure.** Drafts produced with AI assistance are
   scaffolding: the human author must understand, verify, and substantially
   rewrite them before submission, and must check the target venue's AI-use
   policy (most IEEE/ACM venues require disclosure and hold authors fully
   responsible for content). The `learn/` course exists so the author can
   independently defend every design and every number.
3. **Reproducibility is the selling point.** Anything that produced a number in
   the paper must be reproducible via `scripts/reproduce.sh` from a clean
   checkout. CSVs in `data/` are committed; figures are regenerated, never
   hand-edited.
4. **Honest methodology beats big numbers.** Fanless M2 → thermal interleaving
   is mandatory; no thread pinning on macOS → QoS steering is best-effort and
   logged; steady_clock resolves ~41 ns → report means alongside quantized
   percentiles where relevant.
