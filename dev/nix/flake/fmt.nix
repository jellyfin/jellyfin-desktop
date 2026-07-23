{ inputs, ... }: {
  imports = [ inputs.treefmt-nix.flakeModule ];
  perSystem = { ... }: {
    treefmt = {
      projectRootFile = "flake.nix";
      programs = {
        nixfmt = {
          enable = true;
          strict = true;
        };
        rustfmt.enable = true;
        prettier.enable = true; # js+css
      };
    };
  };
}
