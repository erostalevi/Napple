# Offline plan test: the AWS provider is mocked, so `tofu test` makes no AWS call and needs no credentials.
#   cd infra/terraform/bootstrap && tofu init -backend=false && tofu test
mock_provider "aws" {
  mock_data "aws_caller_identity" {
    defaults = {
      account_id = "123456789012"
    }
  }
  mock_data "aws_iam_policy_document" {
    defaults = {
      json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}"
    }
  }
}

run "default_names" {
  command = plan

  assert {
    condition     = aws_s3_bucket.state.bucket == "napple-tfstate-123456789012-us-east-1"
    error_message = "state bucket name is not derived from the account id and region"
  }
  assert {
    condition     = aws_dynamodb_table.lock.hash_key == "LockID"
    error_message = "the S3 backend needs a LockID hash key"
  }
  assert {
    condition     = aws_s3_bucket_public_access_block.state.restrict_public_buckets
    error_message = "the state bucket must block public access"
  }
}

run "explicit_bucket_name" {
  command = plan
  variables {
    state_bucket_name = "my-napple-state"
    region            = "eu-west-1"
  }
  assert {
    condition     = aws_s3_bucket.state.bucket == "my-napple-state"
    error_message = "state_bucket_name is ignored"
  }
}
