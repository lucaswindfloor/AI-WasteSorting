"""Tests for the optional PyTorch BCAF checkpoint loader."""

import unittest

import torch
from torch import nn

from sim.m01.bcaf_infer import normalize_checkpoint


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.model = nn.Module()
        for branch in ("rgb_model", "hsi_model"):
            sensor = nn.Module()
            sensor.backbone = nn.Sequential(nn.Linear(2, 3, bias=False))
            self.model.add_module(branch, sensor)
        self.model.seg_head = nn.Linear(3, 2, bias=False)
        self.expected = self.model.state_dict()
        self.checkpoint = {
            "module._orig_mod.rgb_model.0.weight": torch.randn(3, 2),
            "hsi_model.0.weight": torch.randn(3, 2),
            "seg_head.weight": torch.randn(2, 3),
        }

    def test_released_and_current_key_layouts(self):
        mapped = normalize_checkpoint({"model": self.checkpoint}, self.expected.keys())
        self.model.load_state_dict(mapped, strict=True)
        torch.testing.assert_close(self.model.rgb_model.backbone[0].weight,
                                   self.checkpoint["module._orig_mod.rgb_model.0.weight"])
        torch.testing.assert_close(self.model.hsi_model.backbone[0].weight,
                                   self.checkpoint["hsi_model.0.weight"])
        canonical = normalize_checkpoint(mapped, self.expected.keys())
        self.model.load_state_dict(canonical, strict=True)
        self.assertEqual(set(canonical), set(mapped))

    def test_duplicate_alias_is_rejected(self):
        self.checkpoint["rgb_model.backbone.0.weight"] = torch.randn(3, 2)
        with self.assertRaisesRegex(ValueError, "duplicate parameter"):
            normalize_checkpoint(self.checkpoint, self.expected.keys())

    def test_incompatible_checkpoints_remain_rejected(self):
        missing = {k: v for k, v in self.checkpoint.items() if k != "seg_head.weight"}
        extra = {**self.checkpoint, "hsi_model.typo.weight": torch.randn(3, 2)}
        wrong_shape = {**self.checkpoint, "hsi_model.0.weight": torch.randn(2, 2)}
        for checkpoint, error in ((missing, "Missing key"), (extra, "Unexpected key"),
                                  (wrong_shape, "size mismatch")):
            with self.subTest(error=error), self.assertRaisesRegex(RuntimeError, error):
                mapped = normalize_checkpoint(checkpoint, self.expected.keys())
                self.model.load_state_dict(mapped, strict=True)
