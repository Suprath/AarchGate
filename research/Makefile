# AarchGate Research Paper Makefile
# Requirements: pandoc, pdflatex, python3 (with matplotlib)

PAN = pandoc
TEX = pdflatex
BIB = bibtex

SRC = paper.md
SECTIONS = sections/*.md
APPENDICES = appendices/*.md
FIGURES = figures/*.md

all: pdf

pdf: figures
	$(PAN) $(SRC) -o paper.tex --standalone --natbib --metadata-file=metadata.yaml
	$(TEX) paper.tex
	$(BIB) paper
	$(TEX) paper.tex
	$(TEX) paper.tex
	@echo "Paper built: paper.pdf"

figures:
	python3 figures/generate_charts.py
	@echo "Figures generated."

clean:
	rm -f *.aux *.log *.out *.toc *.bbl *.blg *.tex *.pdf
	rm -f figures/*.png

.PHONY: all pdf figures clean
