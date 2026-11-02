terraform {
  required_version = ">= 1.8.0"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 6.0"
    }
  }
  # No backend block on purpose: the first run keeps a local state, then infra/scripts/bootstrap.sh writes a backend_override.tf pointing at the
  # bucket this stack created and migrates the state into it (key bootstrap/terraform.tfstate). See docs/guides/deploy-servers.md.
}

provider "aws" {
  region = var.region
  default_tags {
    tags = {
      Project   = "napple"
      Stack     = "bootstrap"
      ManagedBy = "opentofu"
    }
  }
}
