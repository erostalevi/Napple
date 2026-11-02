# Offline plan test: the AWS provider is mocked, so `tofu test` makes no AWS call and needs no credentials.
#   cd infra/terraform && tofu init -backend=false && tofu test
mock_provider "aws" {
  mock_data "aws_route53_zone" {
    defaults = {
      zone_id      = "Z0123456789MOCKZONE"
      name_servers = ["ns-1.awsdns-00.org", "ns-2.awsdns-00.com"]
    }
  }
  mock_data "aws_ssm_parameter" {
    defaults = {
      value = "ami-0123456789abcdef0"
    }
  }
  mock_data "aws_iam_policy_document" {
    defaults = {
      json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}"
    }
  }
  mock_resource "aws_ebs_volume" {
    defaults = {
      id = "vol-0123456789abcdef0"
    }
  }
  mock_resource "aws_ecr_repository" {
    defaults = {
      arn = "arn:aws:ecr:us-east-1:123456789012:repository/mock"
    }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = {
      arn = "arn:aws:logs:us-east-1:123456789012:log-group:/napple/servers"
    }
  }
}

run "defaults" {
  command = plan

  assert {
    condition     = aws_route53_record.server.name == "pretend.napple.lat" && aws_route53_record.server.type == "A"
    error_message = "pretend.napple.lat A record missing"
  }
  assert {
    condition     = aws_route53_record.www[0].records == toset(["erostalevi.github.io"])
    error_message = "www must CNAME to <owner>.github.io"
  }
  assert {
    condition     = length(aws_route53_record.apex_a[0].records) == 4 && length(aws_route53_record.apex_aaaa[0].records) == 4
    error_message = "apex needs the four GitHub Pages A and AAAA addresses"
  }
  assert {
    condition     = aws_route53_record.wildcard[0].name == "*.napple.lat"
    error_message = "wildcard record missing"
  }
  assert {
    condition     = length(aws_route53_record.pages_verification) == 0
    error_message = "no verification TXT without a value"
  }
  assert {
    condition     = aws_instance.servers.instance_type == "t4g.small" && aws_instance.servers.metadata_options[0].http_tokens == "required"
    error_message = "instance type or IMDSv2 wrong"
  }
  assert {
    condition     = strcontains(aws_instance.servers.user_data, "nvme-Amazon_Elastic_Block_Store_vol0123456789abcdef0")
    error_message = "user data does not find the data volume by id"
  }
  assert {
    condition     = !contains([for r in aws_vpc_security_group_ingress_rule.v4 : r.from_port], 22)
    error_message = "SSH must stay closed (SSM only)"
  }
  assert {
    condition     = aws_vpc_security_group_ingress_rule.v4["nex"].from_port == 60000 && aws_vpc_security_group_ingress_rule.v4["nex"].ip_protocol == "udp"
    error_message = "NEX UDP range not open"
  }
  assert {
    condition     = aws_ecr_repository.image["napple-online"].image_tag_mutability == "IMMUTABLE"
    error_message = "image tags must be immutable"
  }
}

run "pages_off_and_verification" {
  command = plan
  variables {
    wildcard_to_pages             = false
    github_pages_verification_txt = "abc123"
    github_owner                  = "SomeOrg"
  }
  assert {
    condition     = length(aws_route53_record.wildcard) == 0
    error_message = "wildcard_to_pages = false must drop the wildcard"
  }
  assert {
    condition     = aws_route53_record.pages_verification[0].name == "_github-pages-challenge-someorg.napple.lat"
    error_message = "verification TXT name wrong"
  }
  assert {
    condition     = aws_route53_record.www[0].records == toset(["someorg.github.io"])
    error_message = "owner is not lower-cased in the CNAME target"
  }
}

run "x86_64" {
  command = plan
  variables {
    instance_type = "t3.small"
    architecture  = "x86_64"
  }
  assert {
    condition     = data.aws_ssm_parameter.al2023.name == "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64"
    error_message = "AMI parameter does not follow architecture"
  }
}

run "bad_architecture" {
  command = plan
  variables {
    architecture = "riscv64"
  }
  expect_failures = [var.architecture]
}
