# CI's Docker builds (docker buildx bake), with layers kept in GitHub's Actions cache.
# The Windows image copies the camera SDK headers and fonts out of the Linux image (docker/Dockerfile.mingw's
# UBUNTU_IMAGE), so it gets the "linux" target as a named build context instead of a local image.

target "linux" {
  context    = "docker"
  dockerfile = "Dockerfile"
  tags       = ["slopcap-build:ci"]
  cache-from = ["type=gha,scope=linux"]
}

target "mingw" {
  context    = "docker"
  dockerfile = "Dockerfile.mingw"
  args       = { UBUNTU_IMAGE = "slopcap-build-ci-context" }
  contexts   = { "slopcap-build-ci-context" = "target:linux" }
  tags       = ["slopcap-build-mingw:ci"]
  cache-from = ["type=gha,scope=mingw"]
  cache-to   = ["type=gha,scope=mingw,mode=max"]
}
