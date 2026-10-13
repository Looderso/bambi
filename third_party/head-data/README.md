# The shipped heads' data

Nothing in this folder is compiled. It holds the attribution and the licences of the measured heads the
Decoder's two shipped heads are learned from — HUTUBS and 3D3A (CC BY 4.0) for the learned head, the SADIE II
KU100 (Apache 2.0) for the second — so that `tools/release/notices.py` carries them into the notices every
build ships with. The data itself is not in the repository; `tools/head` and the private pipeline turn it into
`core/data/decode/head.json` and `ku100.json`.
