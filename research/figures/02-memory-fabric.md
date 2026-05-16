# Figure 2: Memory Ingestion and Alignment Pipeline

```latex
\begin{tikzpicture}[node distance=1.5cm]
  \tikzstyle{block} = [rectangle, draw, fill=green!10, text width=5cm, text centered, minimum height=1cm]
  
  \node (shm) [block] {iceoryx Shared Memory (Zero-Copy)};
  \node (flat) [block, below of=shm] {FlatBuffers Schema Layout};
  \node (align) [block, below of=flat] {Page Alignment (4096B)};
  \node (cache) [block, below of=align] {Cache-Line Alignment (64B)};
  \node (slicer) [block, below of=cache, fill=blue!10] {AarchGate Bit-Slicer};
  
  \draw [->] (shm) -- (flat);
  \draw [->] (flat) -- (align);
  \draw [->] (align) -- (cache);
  \draw [->] (cache) -- (slicer);
\end{tikzpicture}
```

### ASCII Fallback
```
[ iceoryx SHM ] ----> [ FlatBuffers Schema ]
                             |
                             v
                    [ Page Alignment (4096B) ]
                             |
                             v
                    [ Cache-Line Alignment (64B) ]
                             |
                             v
                    [ AarchGate Bit-Slicer ]
```
