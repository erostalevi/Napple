output "state_bucket" {
  description = "S3 bucket for OpenTofu state (pass as -backend-config=bucket=...)."
  value       = aws_s3_bucket.state.id
}

output "region" {
  value = var.region
}
