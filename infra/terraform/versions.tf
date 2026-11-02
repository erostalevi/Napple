terraform {
  required_version = ">= 1.8.0"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 6.0"
    }
  }

  # Partial configuration: bucket, key, region and use_lockfile = true are passed by infra/scripts/tofu-servers.sh
  # (-backend-config=...), from the bootstrap stack's names. Offline checks use `tofu init -backend=false`.
  backend "s3" {}
}

provider "aws" {
  region = var.region
  default_tags {
    tags = {
      Project   = "napple"
      Stack     = "servers"
      ManagedBy = "opentofu"
    }
  }
}
