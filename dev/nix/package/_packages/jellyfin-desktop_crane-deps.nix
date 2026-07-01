{
  craneLib,
  craneCommonArgs,
  metaSkeleton,
}:
craneLib.buildDepsOnly (
  craneCommonArgs
  // {
    meta = metaSkeleton // {
      description = "${metaSkeleton.description} - dependencies";
    };
  }
)
