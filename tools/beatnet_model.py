"""The BeatNet+ network, restated from BeatNet+ so the tools can run it without it.

This is ``BeatNetPlusBranch`` from src/BeatNetPlus/model.py at commit
bb90eb0a9065b101a4b4c4cb2b2061950266cb4b — the single branch the published weight sets
hold and the only one inference ever uses — copied layer for layer, with the training
and dual-branch machinery around it left out. tools/beatnet_features.py does the same
for the feature front end, and for the same reason: the upstream package pulls in
pyaudio and its own madmom pin, and nothing here needs either.

The shapes are not taken on trust from HANDOFF §5.3. They are checked against the
state_dict of whichever .pt is loaded, and the parameter count is checked against the
file: 767,125 parameters, of which the four LSTM layers are 724,800.

    288 features
      -> Conv1d(1, 2, kernel_size=10)                             22 params
      -> ReLU -> MaxPool1d(2)                        2 x 139 = 278 values
      -> Linear(278, 150)                                     41,850
      -> LSTM(150, 150, num_layers=4, batch_first)            724,800
      -> Linear(150, 3)                                          453
      -> softmax over the three classes           P(beat), P(downbeat), P(non-beat)

Two details the C++ port has to match and neither diagram mentions. MaxPool1d(2) over
279 convolution outputs keeps 139 and *discards the last one*, and the flatten that
follows is PyTorch's row-major ``view``: filter 0's 139 values, then filter 1's. Get
either wrong and Linear(278, 150) is fed a permutation of the right numbers.
"""

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

FEATURE_DIM = 288
NUM_CELLS = 150
NUM_LAYERS = 4
NUM_CLASSES = 3
KERNEL_SIZE = 10
CONV_FILTERS = 2
CONV_FLAT_DIM = CONV_FILTERS * ((FEATURE_DIM - KERNEL_SIZE + 1) // 2)  # 278
TOTAL_PARAMS = 767_125

# name -> shape, the whole state_dict of a branch, in the order it is stored.
PARAMETER_SHAPES = {
    "conv1.weight": (CONV_FILTERS, 1, KERNEL_SIZE),
    "conv1.bias": (CONV_FILTERS,),
    "linear0.weight": (NUM_CELLS, CONV_FLAT_DIM),
    "linear0.bias": (NUM_CELLS,),
    **{
        f"lstm.{kind}_l{layer}": shape
        for layer in range(NUM_LAYERS)
        for kind, shape in (
            # Layer 0's input is linear0's 150 outputs, so every layer is 150 -> 150.
            ("weight_ih", (4 * NUM_CELLS, NUM_CELLS)),
            ("weight_hh", (4 * NUM_CELLS, NUM_CELLS)),
            ("bias_ih", (4 * NUM_CELLS,)),
            ("bias_hh", (4 * NUM_CELLS,)),
        )
    },
    "output_linear.weight": (NUM_CLASSES, NUM_CELLS),
    "output_linear.bias": (NUM_CLASSES,),
}


class BeatNetPlusBranch(nn.Module):
    """One BeatNet+ branch. Same module names as upstream, so a .pt loads unchanged."""

    def __init__(self, dim_in=FEATURE_DIM, num_cells=NUM_CELLS, num_layers=NUM_LAYERS):
        super().__init__()
        self.dim_in = dim_in
        self.num_cells = num_cells
        self.num_layers = num_layers
        self.kernel_size = KERNEL_SIZE
        self.conv_out = num_cells

        self.conv1 = nn.Conv1d(1, CONV_FILTERS, self.kernel_size)
        conv_flat_dim = CONV_FILTERS * ((dim_in - self.kernel_size + 1) // 2)
        self.linear0 = nn.Linear(conv_flat_dim, self.conv_out)
        self.lstm = nn.LSTM(input_size=self.conv_out, hidden_size=num_cells,
                            num_layers=num_layers, batch_first=True, bidirectional=False)
        self.output_linear = nn.Linear(num_cells, NUM_CLASSES)

        self.hidden = torch.zeros(num_layers, 1, num_cells)
        self.cell = torch.zeros(num_layers, 1, num_cells)

    def _extract_features(self, data):
        B, T, D = data.shape
        x = data.reshape(-1, D).unsqueeze(1)        # (B*T, 1, dim_in)
        x = F.max_pool1d(F.relu(self.conv1(x)), 2)  # (B*T, 2, (dim_in - 9) // 2)
        x = x.view(x.size(0), -1)                   # (B*T, conv_flat_dim), filter-major
        x = self.linear0(x)
        return x.reshape(B, T, self.conv_out)

    def forward(self, data):
        """Streaming: carries the LSTM state from call to call, as the C++ side does."""
        x = self._extract_features(data)
        x, (self.hidden, self.cell) = self.lstm(x, (self.hidden, self.cell))
        return self.output_linear(x).transpose(1, 2)  # (B, 3, T)

    def inference_forward(self, data):
        """The whole sequence at once from a zero state, which is what BeatNet+'s
        'online' mode uses. Identical to resetting and calling forward() frame by
        frame; tools/model_reference.py checks that rather than assuming it."""
        x = self._extract_features(data)
        x = self.lstm(x)[0]
        return self.output_linear(x).transpose(1, 2)  # (B, 3, T)

    def reset_hidden(self):
        self.hidden = torch.zeros(self.num_layers, 1, self.num_cells)
        self.cell = torch.zeros(self.num_layers, 1, self.num_cells)


def load_state_dict(path):
    """The state_dict of a published .pt, with every shape and the total checked."""
    state = torch.load(path, map_location="cpu", weights_only=True)
    if not isinstance(state, dict):
        raise SystemExit(f"{path}: expected a state_dict, got {type(state).__name__}")
    got = {name: tuple(tensor.shape) for name, tensor in state.items()}
    if got != PARAMETER_SHAPES:
        missing = sorted(set(PARAMETER_SHAPES) - set(got))
        extra = sorted(set(got) - set(PARAMETER_SHAPES))
        wrong = sorted(n for n in set(got) & set(PARAMETER_SHAPES) if got[n] != PARAMETER_SHAPES[n])
        raise SystemExit(f"{path}: not a BeatNet+ branch — missing {missing}, unexpected {extra}, "
                         f"wrong shapes {[(n, got[n], PARAMETER_SHAPES[n]) for n in wrong]}")
    for name, tensor in state.items():
        if tensor.dtype != torch.float32:
            raise SystemExit(f"{path}: {name} is {tensor.dtype}, expected float32")
    total = sum(int(np.prod(shape)) for shape in got.values())
    if total != TOTAL_PARAMS:
        raise SystemExit(f"{path}: {total} parameters, expected {TOTAL_PARAMS}")
    return state


def load_branch(path):
    """A branch in eval mode holding the weights in `path`."""
    model = BeatNetPlusBranch()
    model.load_state_dict(load_state_dict(path))
    model.eval()
    return model
