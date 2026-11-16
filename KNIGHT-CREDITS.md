# Knight demo: attribution and licensing notice

This notice concerns the **Animated Knight** demonstration content, including
the `knight*.pak` files published under Tourmaline's rolling `demo-content`
release and any Knight screenshots in the Tourmaline README. It does **not**
change the license of the Tourmaline engine or grant any new rights in the
model.

## Original asset

The model is the **Animated Knight**, one of the professionally created
assets in Intel's **GPU Research Samples Library** (a fully rigged character
with example animations; source shipped as Autodesk Maya, Alembic (ABC), FBX
and USD):

- **Title** — Animated Knight, Intel GPU Research Samples Library.
- **Author / copyright** — Intel Corporation.
- **Origin** — <https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html>
- **Citation** — if used in a publication, cite the Intel Sample Library:

  ```bibtex
  @misc{intel-samples,
    Author = {Intel Corporation},
    Year = {2022},
    Note = {https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html},
    Title = {Intel Sample Library}
  }
  ```

## License

Downloads from the Intel GPU Research Samples Library are made available
under the **Creative Commons Attribution 4.0 International (CC BY 4.0)**
license. Redistribution is permitted provided the attribution above is
retained.

## Tourmaline-specific work

- **Samuel Martin (Tourmaline maintainer)** — converted the source to glTF
  (Blender glTF I/O v4.5) for the Tourmaline pipeline and re-bound the
  deform-joint constraints to the evaluated pose.
- **Tourmaline project** — import, meshlet cook (`--meshlet-split-factor
  2.0`) and packaging into the engine's `.pak` demonstration format.

These processing and packaging credits **do not replace** the attribution
attached to the original rig, which remains with Intel under CC BY 4.0.

This is a provenance and credits record, **not legal advice**.
