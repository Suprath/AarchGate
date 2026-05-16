# Figure 1: AarchGate Architectural Flow

```latex
\begin{tikzpicture}[node distance=2cm]
  % Define styles
  \tikzstyle{box} = [rectangle, minimum width=3cm, minimum height=1cm, text centered, draw=black, fill=blue!10]
  \tikzstyle{arrow} = [thick,->,>=stealth]
  
  % Nodes
  \node (input) [box] {AoS Data (Rows)};
  \node (slicer) [box, below of=input] {Bit-Slicer (Knuth Butterfly)};
  \node (planes) [box, below of=slicer] {64 Bit-Planes};
  \node (jit) [box, right of=planes, xshift=4cm] {JIT Logic Kernel};
  \node (result) [box, above of=jit] {64-bit Result Mask};
  
  % Arrows
  \draw [arrow] (input) -- (slicer);
  \draw [arrow] (slicer) -- (planes);
  \draw [arrow] (planes) -- (jit);
  \draw [arrow] (jit) -- (result);
  
  % Descriptions
  \node[right of=input, xshift=2cm] {Row-major (Unstructured)};
  \node[right of=planes, xshift=1cm] {Transposed (Circuit-Ready)};
  \node[below of=jit, yshift=1cm] {Branchless ARM64 ASM};
\end{tikzpicture}
```

### ASCII Fallback
```
[ AoS Data (Rows) ]
        |
        v
[ Bit-Slicer (Knuth) ]
        |
        v
[ 64 Bit-Planes ] ----> [ JIT Logic Kernel ]
                                |
                                v
                      [ 64-bit Result Mask ]
```
