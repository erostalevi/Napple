output "state_bucket" {
  description = "S3 bucket for OpenTofu state (pass as -backend-config=bucket=...)."
  value       = aws_s3_bucket.state.id
}

output "lock_table" {
  description = "DynamoDB lock table (pass as -backend-config=dynamodb_table=...)."
  value       = aws_dynamodb_table.lock.name
}

output "region" {
  value = var.region
}
