variable "region" {
  description = "AWS region of the state bucket (the servers stack uses the same one by default)."
  type        = string
  default     = "us-east-1"
}

variable "state_bucket_name" {
  description = "Name of the S3 bucket for OpenTofu state. Empty = napple-tfstate-<account id>-<region> (globally unique without coordination)."
  type        = string
  default     = ""
}

variable "noncurrent_version_days" {
  description = "Days an overwritten state version is kept (rollback window for a bad apply)."
  type        = number
  default     = 90
}
