# UNBSE documentation API

This directory is the source for the public, read-only plugin documentation API.
Consumers should begin with `index.json`, follow its current version, and avoid
constructing undocumented paths.

Existing versioned fields and resources remain stable. Additive SDK changes stay
under `v1`; incompatible documentation contract changes receive a new API version.
Release and interface metadata must be updated in the same change as the public
manifest or SDK headers. The publication workflow copies the canonical headers
from `include/` into the hosted `api/v1/headers/` path.
