# Sponza demo: attribution and licensing notice

This notice concerns **Sponza-derived demonstration content**, including the
`sponza_10.tapp` / `sponza_10-*.pak` files published under Tourmaline's
[rolling `demo-content` release](https://github.com/striker-samuel/tourmaline/releases/tag/demo-content),
and the Sponza screenshots in the Tourmaline README. It does **not** change
the license of the Tourmaline engine or grant any new rights in the model.

## Original asset and contributors

The Tourmaline maintainer identifies the starting point as the Sponza glTF
model in [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza),
subsequently modified and processed for the Tourmaline demo.

The [Khronos Sponza documentation](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Sponza/README.md)
records the following source history and credits:

- **Marko Dabrovic** — original Sponza model (2002).
- **Frank Meinl / Crytek** — the revised Sponza Atrium model (2010).
  The [Khronos model metadata](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Sponza/metadata.json)
  identifies **Crytek** as the copyright owner (© 2016).
- **Morgan McGuire** — modifications to Crytek's OBJ export (2011),
  as recorded in the upstream texture archive's copyright notes.
- **Alexandre Pestana** — Sponza PBR texture work used by the Khronos
  version; see the [source notes](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Sponza/README.md).
- **Khronos glTF Sample Assets contributors** — the published glTF
  adaptation and documented texture/material processing. The upstream
  model metadata does not identify an individual glTF adapter, so no
  individual authorship is asserted here.

## Tourmaline-specific work

- **Samuel Martin (Tourmaline maintainer)** — performed the conversion
  of source textures to KTX for this demo, per the maintainer's account.
- **Tourmaline project** — scene adjustments and conversion/cooking into
  the engine's packaged `.tapp` / `.pak` demonstration format.

These processing and packaging credits **do not replace** the rights or
attribution attached to the original geometry and textures.

## License status — important

The Khronos model's [per-model LICENSE.md](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Sponza/LICENSE.md)
identifies the **CRYENGINE Limited License Agreement** for model-associated
text, images and binary files. Its **CC BY 4.0** designation covers the
license and other *metadocumentation*, **not** the model or textures as a whole.
The upstream [license reference](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/LICENSES/LicenseRef-CRYENGINE-Agreement.txt)
points to [Crytek's terms](https://www.cryengine.com/ce-terms).

An [open Khronos issue (#172)](https://github.com/KhronosGroup/glTF-Sample-Assets/issues/172)
raises a specific concern about whether that Crytek-derived asset may
be redistributed outside CRYENGINE. This is a **reported, unresolved
licensing concern**, not a legal ruling. The attribution here is **not**
a claim that redistribution of Tourmaline's cooked Sponza pak is
authorized, and packaging the data or converting textures to KTX
does not change the upstream rights.

Before any new publication of the demo, verify the exact source
files, any independent texture licenses, and permission to
redistribute the resulting content. If needed, replace it with
a source whose license explicitly covers the intended use.

This is a provenance and credits record, **not legal advice**.
