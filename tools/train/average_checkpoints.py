"""Average the weights of several checkpoints of one run into one — a "model soup".

    python tools/train/average_checkpoints.py build/training/runs/electronic-library-v2/epoch_016.pt \
        build/training/runs/electronic-library-v2/epoch_024.pt \
        build/training/runs/electronic-library-v2/epoch_032.pt --out .../soup-16-32.pt

Checkpoints of one fine-tune, a few epochs apart, sit in one basin; the mean of their
weights is usually a little better than any of them on held-out data and a lot steadier
(Wortsman et al. 2022, and stochastic weight averaging before it). It costs nothing to
try: the result is a state dict `tools/convert_weights.py --name` turns into a set like
any other, and the gates say whether it earned its place. Only state dicts of the same
architecture, from `finetune.py`'s `best.pt` / `last.pt` / `epoch_NNN.pt`, are averaged;
a `resume.pt` (model + optimizer) is unwrapped.
"""
import argparse
import sys
from pathlib import Path


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("checkpoints", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)
    import torch
    states = []
    for p in a.checkpoints:
        ck = torch.load(p, map_location="cpu", weights_only=False)
        states.append(ck["model"] if isinstance(ck, dict) and "model" in ck and "optimizer" in ck else ck)
    keys = list(states[0].keys())
    for s in states[1:]:
        if list(s.keys()) != keys:
            raise SystemExit("the checkpoints do not share one set of parameters")
    mean = {}
    for k in keys:
        stack = torch.stack([s[k].float() for s in states])
        mean[k] = stack.mean(0).to(states[0][k].dtype)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    torch.save(mean, a.out)
    n = sum(v.numel() for v in mean.values())
    print(f"{len(states)} checkpoints averaged -> {a.out} ({n} parameters)")


if __name__ == "__main__":
    main(sys.argv[1:])
