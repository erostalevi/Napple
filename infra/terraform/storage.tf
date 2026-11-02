# Private S3 buckets of the account service: profile photos (napple-account re-encodes every upload to a 256x256 JPEG and serves it behind the
# tier rules, so the bucket is never public) and E2E-encrypted cloud-save blobs. Block Public Access on, TLS only, SSE-S3, no versioning (a
# deleted account's objects must really go). Bucket names carry the account id and region (global namespace), like the state bucket.
data "aws_caller_identity" "current" {}

locals {
  account_id    = data.aws_caller_identity.current.account_id
  photo_bucket  = "napple-photos-${local.account_id}-${var.region}"
  saves_bucket  = "napple-saves-${local.account_id}-${var.region}"
  data_buckets  = { photos = local.photo_bucket, saves = local.saves_bucket }
  ses_identity  = "arn:aws:ses:${var.region}:${local.account_id}:identity/${var.domain}"
  ses_configset = "arn:aws:ses:${var.region}:${local.account_id}:configuration-set/${var.ses_config_set}"
}

resource "aws_s3_bucket" "data" {
  for_each      = local.data_buckets
  bucket        = each.value
  force_destroy = var.ecr_force_delete
}

resource "aws_s3_bucket_public_access_block" "data" {
  for_each                = local.data_buckets
  bucket                  = aws_s3_bucket.data[each.key].id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_ownership_controls" "data" {
  for_each = local.data_buckets
  bucket   = aws_s3_bucket.data[each.key].id
  rule {
    object_ownership = "BucketOwnerEnforced"
  }
}

resource "aws_s3_bucket_server_side_encryption_configuration" "data" {
  for_each = local.data_buckets
  bucket   = aws_s3_bucket.data[each.key].id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "data" {
  for_each = local.data_buckets
  bucket   = aws_s3_bucket.data[each.key].id
  rule {
    id     = "abort-incomplete-uploads"
    status = "Enabled"
    filter {}
    abort_incomplete_multipart_upload {
      days_after_initiation = 2
    }
  }
}

data "aws_iam_policy_document" "tls_only" {
  for_each = local.data_buckets
  statement {
    sid       = "DenyInsecureTransport"
    effect    = "Deny"
    actions   = ["s3:*"]
    resources = [aws_s3_bucket.data[each.key].arn, "${aws_s3_bucket.data[each.key].arn}/*"]
    principals {
      type        = "*"
      identifiers = ["*"]
    }
    condition {
      test     = "Bool"
      variable = "aws:SecureTransport"
      values   = ["false"]
    }
  }
}

resource "aws_s3_bucket_policy" "tls_only" {
  for_each   = aws_s3_bucket.data
  bucket     = each.value.id
  policy     = data.aws_iam_policy_document.tls_only[each.key].json
  depends_on = [aws_s3_bucket_public_access_block.data]
}
