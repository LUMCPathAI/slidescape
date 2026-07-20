# Captured DICOMweb response shapes

These small, de-identified fixtures preserve response shapes observed from the NCI
Imaging Data Commons public viewer proxy on 2026-07-19. They are deliberately
reduced to attributes used by Slidescape so tests do not depend on a live public
service or mutable IDC release metadata.

`idc_scoped_instances.json` captures an important scoped-QIDO behavior: the response
contains SOP Instance UIDs but omits the Study and Series Instance UIDs already fixed
by the request path.

`idc_volume_metadata.json` captures the relevant metadata shape for one `TILED_FULL`
brightfield `VOLUME` instance from that series. Pixel bytes are not included.

Source identifiers:

- Study: `2.25.211094631316408413440371843585977094852`
- Series: `1.3.6.1.4.1.5962.99.1.208792987.352384958.1640886332827.2.0`

Live IDC checks remain optional; these files are the offline parser regressions.
