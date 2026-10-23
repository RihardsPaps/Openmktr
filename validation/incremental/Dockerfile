# Copyright 2026 Rihards Paps and Haralds Paps
# SPDX-License-Identifier: MPL-2.0
# External comparison image only. Never use this as the portable test image.
ARG BASE_IMAGE=openmktr-test
FROM ${BASE_IMAGE}
RUN apk add gmake
ENTRYPOINT ["python", "validation/incremental.py"]
CMD ["--make", "gmake", "--omktr", "/workspace/omktr", "--output", "/evidence/campaign"]
