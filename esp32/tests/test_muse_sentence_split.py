# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


"""Where spoken replies are cut into sentences as their text streams in
(muse_text_sentence_end): never inside a word still arriving, a number or a
UTF-8 character, and closing quotes stay with the sentence they close."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_text.h"

int main(int argc, char **argv)
{
    (void)argc;
    printf("%zu\n", muse_text_sentence_end(argv[1], strlen(argv[1]), (size_t)atoi(argv[2])));
    return 0;
}
"""


class SentenceSplitTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        harness = Path(cls.tmp.name) / "sentence_harness.c"
        harness.write_text(HARNESS)
        cls.binary = Path(cls.tmp.name) / "sentence_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(harness),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o",
                str(cls.binary),
            ],
            capture_output=True,
            text=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def first(self, text: str, min_bytes: int = 0) -> str:
        """The first sentence, as text; "" when there's none yet."""
        out = subprocess.run([str(self.binary), text, str(min_bytes)], capture_output=True, check=True)
        n = int(out.stdout)
        return text.encode()[:n].decode()

    def test_english_needs_the_space_after(self) -> None:
        self.assertEqual(self.first("Hello there. How are"), "Hello there. ")
        self.assertEqual(self.first("Hello there."), "")          # no space yet: "there.com"?
        self.assertEqual(self.first("Pi is 3.14 and"), "")
        self.assertEqual(self.first("Really?! Yes"), "Really?! ")
        self.assertEqual(self.first('He said "go." Then'), 'He said "go." ')

    def test_newline_ends_a_sentence(self) -> None:
        self.assertEqual(self.first("A list:\n- one"), "A list:\n")

    def test_cjk_ends_without_a_space(self) -> None:
        self.assertEqual(self.first("你好。我是小智"), "你好。")
        self.assertEqual(self.first("真的吗？"), "真的吗？")
        self.assertEqual(self.first("他说：“好的。”然后"), "他说：“好的。”")
        self.assertEqual(self.first("嗯……好吧"), "嗯……")
        self.assertEqual(self.first("你好，我是小智"), "")          # a comma isn't the end

    def test_min_skips_short_sentences(self) -> None:
        self.assertEqual(self.first("Hi. I am Muse. Ok", 10), "Hi. I am Muse. ")
        self.assertEqual(self.first("好。好的。我是", 7), "好。好的。")
        self.assertEqual(self.first("Hi. There", 10), "")

    def test_partial_utf8_is_never_cut(self) -> None:
        cut = "你好。".encode()[:-1]                               # 。 still arriving
        out = subprocess.run([str(self.binary), cut, "0"], capture_output=True, check=True)
        self.assertEqual(int(out.stdout), 0)


if __name__ == "__main__":
    unittest.main()
