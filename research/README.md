# AarchGate Research Paper Workspace

This folder contains the complete research paper, benchmarks, and visualization scripts for **AarchGate**.

## Structure
- `paper.md`: Master document (GitHub-flavored Markdown).
- `sections/`: Individual chapters (Introduction, Memory Fabric, etc.).
- `figures/`: TikZ source code and generated PNG charts.
- `benchmarks/`: Standalone C++ source code for micro-benchmarking.
- `appendices/`: Technical methodology and cycle-budget proofs.
- `references.bib`: IEEE-style bibliography (50+ citations).

## Building the Paper
To build the final PDF (requires Pandoc and LaTeX):
```bash
make pdf
```

To regenerate performance charts (requires Python 3 and Matplotlib):
```bash
make figures
```

## Running Benchmarks
To execute the micro-benchmarks and verify performance claims:
```bash
cd benchmarks
# Compile and run
g++ -O3 bench_slicer.cpp -I../include -o bench_slicer
./bench_slicer
```

## Abstract
See [sections/00-abstract.md](sections/00-abstract.md) for the high-level summary of the paper's contributions.
