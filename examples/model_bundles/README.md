# Reference model bundles

These manifests describe checked ESP-DL 3.3.13 models for ESP32-S3. Model files
remain external to these examples; the firmware does not embed them.

| Manifest directory | Model | Input/result adapters |
| --- | --- | --- |
| `imagenet_mobilenetv2` | MobileNetV2 ImageNet classifier | Image / classification |
| `pedestrian_pico` | Single-class PICO pedestrian detector | Image / PICO detection |
| `arithmetic` | Original two-input arithmetic fixture | Tensor / raw |

The classifier and detector come from Espressif's ESP-DL model packages, pinned
to the model files and licenses identified by the manifest hashes. See the
[ESP-DL v3.3.13 models](https://github.com/espressif/esp-dl/tree/v3.3.13/models).
The original arithmetic model is in
`tests/fixtures/inference/arithmetic_int8.espdl`; its generator and expected
values are in the same directory. A runtime version and matching hashes do not
replace numerical validation for a new model.

## Build and install

Collect each model and its declared assets under one directory, preserving the
relative filenames from its manifest. For example, collect the classifier's
`.espdl`, labels, and `LICENSE` before running:

```sh
python3 scripts/espdl/build_bundle.py examples/model_bundles/imagenet_mobilenetv2/bundle.json \
  --assets /path/to/classifier-assets --output /tmp/imagenet-bundle
python3 scripts/espdl/build_bundle.py examples/model_bundles/arithmetic/bundle.json \
  --assets tests/fixtures/inference --output /tmp/arithmetic-bundle
```

Run these commands from the firmware repository root. The destination must not
exist. The builder validates the manifest and hashes, and copies only declared
files. It does not download, convert, or execute a model.

Copy a completed bundle directory to device storage, for example
`/dl/models/imagenet/`. Copy `examples/python/model_bundle.py` and
`examples/python/infer.py` together into `/dl/`. The helper module is separate
from the built-in `solaros.inference` API.

```text
python /dl/infer.py /dl/models/imagenet/bundle.json /pictures/cat.png
python /dl/infer.py /dl/models/pedestrian/bundle.json /pictures/people.png --count 3
python /dl/infer.py /dl/models/pedestrian/bundle.json stream:camera0 --count 20
```

For the arithmetic bundle, provide an input map such as
`{"a":"a.bin","b":"b.bin"}` and two eight-byte int8 tensors. Relative input
paths resolve beside the map. The same runner supports non-image models and
multiple named inputs.

See [Model bundles and pipelines](../../doc/manual/model-bundles.md) for the
contract, native preprocessing, adapter extension points, execution modes,
RTSP input, frame ownership, and JSON output fields.
