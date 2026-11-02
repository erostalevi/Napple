# Instance role: SSM (Session Manager + send-command for deploys), pull of the two ECR repositories, writing the container log group. Nothing else.
data "aws_iam_policy_document" "ec2_assume" {
  statement {
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "instance" {
  name               = "${local.name}-instance"
  assume_role_policy = data.aws_iam_policy_document.ec2_assume.json
}

resource "aws_iam_role_policy_attachment" "ssm" {
  role       = aws_iam_role.instance.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

data "aws_iam_policy_document" "instance" {
  statement {
    sid       = "EcrToken"
    actions   = ["ecr:GetAuthorizationToken"]
    resources = ["*"]
  }
  statement {
    sid = "EcrPull"
    actions = [
      "ecr:BatchCheckLayerAvailability",
      "ecr:BatchGetImage",
      "ecr:GetDownloadUrlForLayer",
    ]
    resources = [for r in aws_ecr_repository.image : r.arn]
  }
  statement {
    sid       = "Logs"
    actions   = ["logs:CreateLogStream", "logs:PutLogEvents", "logs:DescribeLogStreams"]
    resources = ["${aws_cloudwatch_log_group.servers.arn}:*"]
  }
  # Confirmation / reset code mail (napple-account, SES v2 SendEmail). Only from the domain identity, only through the transactional
  # configuration set (bounces and complaints feed the suppression list). SendRawEmail is the same API family for multipart mail.
  statement {
    sid       = "SesSend"
    actions   = ["ses:SendEmail", "ses:SendRawEmail"]
    resources = [local.ses_identity, local.ses_configset]
  }
  # Profile photos and save blobs: object access only on the two private buckets.
  statement {
    sid       = "DataBucketObjects"
    actions   = ["s3:GetObject", "s3:PutObject", "s3:DeleteObject", "s3:AbortMultipartUpload"]
    resources = [for k, _ in local.data_buckets : "${aws_s3_bucket.data[k].arn}/*"]
  }
  statement {
    sid       = "DataBucketList"
    actions   = ["s3:ListBucket", "s3:GetBucketLocation"]
    resources = [for k, _ in local.data_buckets : aws_s3_bucket.data[k].arn]
  }
}

resource "aws_iam_role_policy" "instance" {
  name   = "${local.name}-instance"
  role   = aws_iam_role.instance.id
  policy = data.aws_iam_policy_document.instance.json
}

resource "aws_iam_instance_profile" "instance" {
  name = "${local.name}-instance"
  role = aws_iam_role.instance.name
}
